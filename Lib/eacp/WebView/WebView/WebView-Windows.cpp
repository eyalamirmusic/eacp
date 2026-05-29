#include <eacp/Core/Utils/WinInclude.h>

#include "WebView.h"
#include "StreamingRange.h"
#include <eacp/Graphics/Helpers/StringUtils-Windows.h>

#include <algorithm>
#include <ea_data_structures/Structures/Vector.h>
#include <cassert>
#include <unordered_map>
#include <queue>
#include <functional>
#include <string>
#include <vector>

#include <objbase.h>
#include <shlwapi.h>

#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Logging.h>

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

HWND findHostHwndForView(View* view);

using MessageHandlerMap =
    std::unordered_map<std::string, std::function<void(const std::string&)>>;

struct CoTaskMemString
{
    LPWSTR ptr = nullptr;

    ~CoTaskMemString()
    {
        if (ptr)
            CoTaskMemFree(ptr);
    }

    LPWSTR* operator&() { return &ptr; }
    operator LPWSTR() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }

    std::string toString() const
    {
        if (!ptr)
            return "";
        return fromWideString(ptr);
    }
};

namespace
{
constexpr double minZoomLevel = 0.25;
constexpr double maxZoomLevel = 5.0;
constexpr double zoomStep = 1.1;

EA::Vector<WebView*>& registeredWebViews()
{
    static auto views = EA::Vector<WebView*>();
    return views;
}

void registerWebView(WebView* view)
{
    registeredWebViews().add(view);
}

void unregisterWebView(WebView* view)
{
    registeredWebViews().removeAllMatches(view);
}

// Strip the outer JSON-string layer that WebView2's ExecuteScript adds.
// macOS's evaluateJavaScript returns the unwrapped string directly, so
// without this every string result on Windows would arrive double-encoded
// (`"abc"` instead of `abc`).
std::string unwrapJsonString(const std::string& raw)
{
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"')
        return raw;

    auto out = std::string {};
    out.reserve(raw.size() - 2);

    for (auto i = std::size_t {1}; i + 1 < raw.size(); ++i)
    {
        auto c = raw[i];
        if (c != '\\')
        {
            out.push_back(c);
            continue;
        }

        if (i + 2 >= raw.size())
            return raw;

        auto esc = raw[++i];
        switch (esc)
        {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
            {
                if (i + 4 >= raw.size())
                    return raw;

                auto hex = raw.substr(i + 1, 4);
                auto code = static_cast<unsigned>(std::stoul(hex, nullptr, 16));
                i += 4;

                // Naive UTF-8 encode of the BMP code point. Enough for ASCII
                // selectors / DOM text; full surrogate-pair handling lives in
                // Miro::Json::parse when callers go that route.
                if (code < 0x80)
                {
                    out.push_back(static_cast<char>(code));
                }
                else if (code < 0x800)
                {
                    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                else
                {
                    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default: return raw;
        }
    }

    return out;
}

// A read-only IStream over a bounded byte range of a StreamingResource.
// WebView2 pulls the response body from this stream, so bytes are fetched
// lazily through ResourceReader::read instead of buffering the whole resource
// up front. The stream owns the resource, so its reader (e.g. an open file)
// stays alive exactly as long as WebView2 keeps reading.
class ReaderStream
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IStream>
{
public:
    ReaderStream(StreamingResource resourceToUse,
                 RangeSize startToUse,
                 RangeSize lengthToUse)
        : resource(std::move(resourceToUse))
        , base(startToUse)
        , length(lengthToUse)
    {
    }

    HRESULT STDMETHODCALLTYPE Read(void* out, ULONG count, ULONG* readOut) override
    {
        auto want = std::min(static_cast<RangeSize>(count), length - position);
        auto got = ULONG {0};

        if (want > 0 && resource.read)
            got = static_cast<ULONG>(
                resource.read(base + position,
                              ByteSpan {static_cast<std::uint8_t*>(out),
                                        static_cast<std::size_t>(want)}));

        position += got;

        if (readOut)
            *readOut = got;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override
    {
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,
                                   DWORD origin,
                                   ULARGE_INTEGER* newPosition) override
    {
        auto target = RangeSize {0};

        switch (origin)
        {
            case STREAM_SEEK_SET:
                target = static_cast<RangeSize>(move.QuadPart);
                break;
            case STREAM_SEEK_CUR:
                target = position + static_cast<RangeSize>(move.QuadPart);
                break;
            case STREAM_SEEK_END:
                target = length + static_cast<RangeSize>(move.QuadPart);
                break;
            default:
                return STG_E_INVALIDFUNCTION;
        }

        position = std::min(target, length);

        if (newPosition)
            newPosition->QuadPart = position;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override
    {
        if (!stat)
            return STG_E_INVALIDPOINTER;

        *stat = STATSTG {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = length;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override
    {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE CopyTo(IStream*,
                                     ULARGE_INTEGER,
                                     ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,
                                         ULARGE_INTEGER,
                                         DWORD) override
    {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,
                                           ULARGE_INTEGER,
                                           DWORD) override
    {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    StreamingResource resource;
    RangeSize base = 0;
    RangeSize length = 0;
    RangeSize position = 0;
};
} // namespace

struct WebView::Native
{
    Native(WebView& ownerToUse, WebView::Options optionsToUse)
        : owner(ownerToUse)
        , options(std::move(optionsToUse))
    {
    }

    ~Native()
    {
        // Unhook our event handlers before tearing the controller down
        // so any late callback from the browser process can't fire
        // back into a half-destroyed Native.
        if (webView)
        {
            if (navigationStartingToken.value)
                webView->remove_NavigationStarting(navigationStartingToken);
            if (navigationCompletedToken.value)
                webView->remove_NavigationCompleted(navigationCompletedToken);
            if (titleChangedToken.value)
                webView->remove_DocumentTitleChanged(titleChangedToken);
            if (webMessageToken.value)
                webView->remove_WebMessageReceived(webMessageToken);
            if (webResourceToken.value)
                webView->remove_WebResourceRequested(webResourceToken);
        }

        if (controller)
            controller->Close();

        // Drop our COM references before DestroyWindow so the WebView2
        // browser-process IPC threads release their connection while
        // the heap is still valid.
        webView.Reset();
        controller.Reset();
        environment.Reset();

        if (childHwnd)
            DestroyWindow(childHwnd);
    }

    void ensureInitialized()
    {
        if (initialized || initInProgress)
            return;

        auto parentHwnd = findHostHwndForView(&owner);
        if (!parentHwnd)
            return;

        initInProgress = true;
        createChildWindow(parentHwnd);
        createWebView2();
    }

    void createChildWindow(HWND parentHwnd)
    {
        static bool classRegistered = false;
        static const wchar_t* CLASS_NAME = L"EACPWebViewHost";

        if (!classRegistered)
        {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = CLASS_NAME;
            RegisterClassExW(&wc);
            classRegistered = true;
        }

        auto bounds = owner.getLocalBounds();
        childHwnd = CreateWindowExW(0,
                                    CLASS_NAME,
                                    L"",
                                    WS_CHILD | WS_VISIBLE,
                                    static_cast<int>(bounds.x),
                                    static_cast<int>(bounds.y),
                                    static_cast<int>(bounds.w),
                                    static_cast<int>(bounds.h),
                                    parentHwnd,
                                    nullptr,
                                    GetModuleHandleW(nullptr),
                                    nullptr);
    }

    void createWebView2()
    {
        if (!childHwnd)
            return;

        auto envOptions = buildEnvironmentOptions();

        auto hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            nullptr,
            envOptions.Get(),
            Microsoft::WRL::Callback<
                ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT
                {
                    if (FAILED(result) || !env)
                    {
                        LOG("WebView2: env create failed hr=0x"
                            + hresultHex(result));
                        initInProgress = false;
                        return result;
                    }

                    environment = env;
                    return env->CreateCoreWebView2Controller(
                        childHwnd,
                        Microsoft::WRL::Callback<
                            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT result,
                                   ICoreWebView2Controller* ctrl) -> HRESULT
                            {
                                if (FAILED(result) || !ctrl)
                                {
                                    LOG("WebView2: controller create failed hr=0x"
                                        + hresultHex(result));
                                    initInProgress = false;
                                    return result;
                                }

                                controller = ctrl;
                                controller->get_CoreWebView2(&webView);

                                applySettings();
                                setupEventHandlers();
                                registerSchemeHandlers();
                                updateBounds();

                                initialized = true;
                                initInProgress = false;

                                processPendingOperations();

                                return S_OK;
                            })
                            .Get());
                })
                .Get());

        if (FAILED(hr))
        {
            LOG("WebView2: CreateCoreWebView2EnvironmentWithOptions failed hr=0x"
                + hresultHex(hr));
            initInProgress = false;
        }
    }

    static std::string hresultHex(HRESULT hr)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08lx", static_cast<unsigned long>(hr));
        return buf;
    }

    Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions>
        buildEnvironmentOptions()
    {
        // Custom URL schemes (anything other than http/https/file) won't
        // navigate at all unless registered here. macOS handles this via
        // WKWebView's setURLSchemeHandler; WebView2 requires both an env-
        // level registration (so the scheme is "real") AND a per-CoreWebView2
        // WebResourceRequested filter (where we actually serve the bytes).
        // Treat the scheme as secure + with an authority component so URLs
        // like app://local/index.html parse the way React / fetch expect.
        auto envOptions = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();

        auto registrations =
            std::vector<Microsoft::WRL::ComPtr<ICoreWebView2CustomSchemeRegistration>> {};
        registrations.reserve(options.schemes.size()
                              + options.streamingSchemes.size());

        auto addRegistration = [&](const std::string& scheme)
        {
            auto wide = toWideString(scheme);
            auto registration =
                Microsoft::WRL::Make<CoreWebView2CustomSchemeRegistration>(
                    wide.c_str());

            registration->put_TreatAsSecure(TRUE);
            registration->put_HasAuthorityComponent(TRUE);

            registrations.push_back(registration);
        };

        for (auto& [scheme, _]: options.schemes)
            addRegistration(scheme);
        for (auto& [scheme, _]: options.streamingSchemes)
            addRegistration(scheme);

        if (!registrations.empty())
        {
            auto raw =
                std::vector<ICoreWebView2CustomSchemeRegistration*>(
                    registrations.size());
            for (auto i = std::size_t {}; i < registrations.size(); ++i)
                raw[i] = registrations[i].Get();

            Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions4> opts4;
            if (SUCCEEDED(envOptions.As(&opts4)) && opts4)
            {
                opts4->SetCustomSchemeRegistrations(
                    static_cast<UINT32>(raw.size()), raw.data());
            }
            else
            {
                LOG("WebView2: ICoreWebView2EnvironmentOptions4 unavailable; "
                    "custom schemes will not navigate");
            }
        }

        Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> base;
        envOptions.As(&base);
        return base;
    }

    void registerSchemeHandlers()
    {
        if (!webView
            || (options.schemes.empty() && options.streamingSchemes.empty()))
            return;

        // Stable owners for the per-scheme provider tables. The
        // WebResourceRequested callback captures `this`, looks the request
        // URL's scheme up here, and dispatches to the matching provider --
        // a one-shot ResourceProvider or a chunked StreamingProvider.
        for (auto& [scheme, provider]: options.schemes)
            schemeProviders.emplace(scheme, provider);
        for (auto& [scheme, provider]: options.streamingSchemes)
            streamingProviders.emplace(scheme, provider);

        webView->add_WebResourceRequested(
            Microsoft::WRL::Callback<
                ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT
                { return handleWebResourceRequested(args); })
                .Get(),
            &webResourceToken);

        // AddWebResourceRequestedFilter only intercepts URLs that match
        // the filter pattern. Use "scheme://*" so every path under the
        // scheme reaches our handler.
        auto addFilter = [&](const std::string& scheme)
        {
            auto pattern = toWideString(scheme + "://*");
            auto hr = webView->AddWebResourceRequestedFilter(
                pattern.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

            if (FAILED(hr))
                LOG("WebView2: AddWebResourceRequestedFilter('" + scheme
                    + "://*') failed hr=0x" + hresultHex(hr));
        };

        for (auto& [scheme, _]: options.schemes)
            addFilter(scheme);
        for (auto& [scheme, _]: options.streamingSchemes)
            addFilter(scheme);
    }

    HRESULT
        handleWebResourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args)
    {
        if (!args || !environment)
            return S_OK;

        Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> request;
        if (FAILED(args->get_Request(&request)) || !request)
            return S_OK;

        CoTaskMemString uriRaw;
        if (FAILED(request->get_Uri(&uriRaw)) || !uriRaw)
            return S_OK;

        auto url = uriRaw.toString();
        auto schemeEnd = url.find("://");
        if (schemeEnd == std::string::npos)
            return S_OK;

        auto scheme = url.substr(0, schemeEnd);

        if (auto streamIt = streamingProviders.find(scheme);
            streamIt != streamingProviders.end() && streamIt->second)
            return handleStreamingRequest(args, request.Get(), url,
                                          streamIt->second);

        auto it = schemeProviders.find(scheme);
        if (it == schemeProviders.end() || !it->second)
            return S_OK;

        auto response = it->second(url);
        Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> webResponse;

        if (!response)
        {
            environment->CreateWebResourceResponse(
                nullptr, 404, L"Not Found",
                L"Content-Type: text/plain; charset=utf-8",
                &webResponse);
            args->put_Response(webResponse.Get());
            return S_OK;
        }

        Microsoft::WRL::ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(response->data.data(),
                                        static_cast<UINT>(response->data.size())));
        if (!stream)
        {
            environment->CreateWebResourceResponse(
                nullptr, 500, L"Internal Server Error",
                L"Content-Type: text/plain; charset=utf-8",
                &webResponse);
            args->put_Response(webResponse.Get());
            return S_OK;
        }

        // CORS bypass: WebView2 enforces cross-origin restrictions even
        // for our own scheme handler, so React's fetch() to a sibling
        // URL fails without an explicit allow-origin header.
        auto headers = std::wstring {L"Content-Type: "}
                       + toWideString(response->mimeType)
                       + L"\r\nAccess-Control-Allow-Origin: *";

        auto reason = (response->statusCode >= 200 && response->statusCode < 300)
                          ? L"OK"
                          : L"Error";

        if (SUCCEEDED(environment->CreateWebResourceResponse(
                stream.Get(), response->statusCode, reason, headers.c_str(),
                &webResponse)))
        {
            args->put_Response(webResponse.Get());
        }

        return S_OK;
    }

    // Serves a custom-scheme request from a StreamingProvider: resolves the
    // request's Range header against the resource size and answers 200 / 206 /
    // 416 with the matching Accept-Ranges / Content-Range / Content-Length
    // headers, streaming the body lazily through a ReaderStream. Mirrors the
    // macOS WKURLSchemeTask streaming path.
    HRESULT handleStreamingRequest(ICoreWebView2WebResourceRequestedEventArgs* args,
                                   ICoreWebView2WebResourceRequest* request,
                                   const std::string& url,
                                   const StreamingProvider& provider)
    {
        auto resource = provider(url);
        ComPtr<ICoreWebView2WebResourceResponse> webResponse;

        if (!resource)
        {
            environment->CreateWebResourceResponse(
                nullptr,
                404,
                L"Not Found",
                L"Content-Type: text/plain; charset=utf-8",
                &webResponse);
            args->put_Response(webResponse.Get());
            return S_OK;
        }

        auto resolved =
            resolveRangeHeader(readRequestHeader(request, L"Range"), resource->size);
        auto served = resolved.served;

        // CORS bypass mirrors the one-shot handler: WebView2 enforces
        // cross-origin rules on our own scheme, so fetch() needs an explicit
        // allow-origin header.
        auto headers =
            std::wstring {L"Content-Type: "} + toWideString(resource->mimeType)
            + L"\r\nAccess-Control-Allow-Origin: *" + L"\r\nAccept-Ranges: bytes";

        auto statusCode = resource->statusCode;

        if (resolved.kind == RangeRequest::Unsatisfiable)
        {
            statusCode = 416;
            headers +=
                L"\r\nContent-Range: bytes */" + std::to_wstring(resource->size);
        }
        else
        {
            if (resolved.kind == RangeRequest::Partial)
            {
                statusCode = 206;
                headers += L"\r\nContent-Range: "
                           + toWideString(contentRangeValue(served, resource->size));
            }

            headers += L"\r\nContent-Length: " + std::to_wstring(served.length);
        }

        ComPtr<IStream> body;
        if (statusCode != 416 && !served.empty())
            body = Microsoft::WRL::Make<ReaderStream>(
                std::move(*resource), served.start, served.length);

        if (SUCCEEDED(
                environment->CreateWebResourceResponse(body.Get(),
                                                       statusCode,
                                                       statusReason(statusCode),
                                                       headers.c_str(),
                                                       &webResponse)))
        {
            args->put_Response(webResponse.Get());
        }

        return S_OK;
    }

    static std::string readRequestHeader(ICoreWebView2WebResourceRequest* request,
                                         LPCWSTR name)
    {
        ComPtr<ICoreWebView2HttpRequestHeaders> headers;
        if (FAILED(request->get_Headers(&headers)) || !headers)
            return {};

        BOOL has = FALSE;
        if (FAILED(headers->Contains(name, &has)) || !has)
            return {};

        CoTaskMemString value;
        if (FAILED(headers->GetHeader(name, &value)) || !value)
            return {};

        return value.toString();
    }

    static LPCWSTR statusReason(int statusCode)
    {
        switch (statusCode)
        {
            case 200:
                return L"OK";
            case 206:
                return L"Partial Content";
            case 416:
                return L"Range Not Satisfiable";
            default:
                return L"OK";
        }
    }

    void applySettings()
    {
        if (!webView)
            return;

        ComPtr<ICoreWebView2Settings> settings;
        webView->get_Settings(&settings);

        if (settings)
        {
            settings->put_AreDevToolsEnabled(options.debugConsole ? TRUE : FALSE);
        }
    }

    void setupEventHandlers()
    {
        if (!webView)
            return;

        webView->add_NavigationStarting(
            Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT
                {
                    CoTaskMemString uri;
                    args->get_Uri(&uri);
                    auto url = uri.toString();
                    Threads::callAsync([cb = owner.onNavigationStarted, url]()
                                       { cb(url); });
                    return S_OK;
                })
                .Get(),
            &navigationStartingToken);

        webView->add_NavigationCompleted(
            Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT
                {
                    BOOL success = FALSE;
                    args->get_IsSuccess(&success);

                    if (success)
                    {
                        CoTaskMemString uri;
                        webView->get_Source(&uri);
                        owner.onNavigationFinished(uri.toString());
                    }
                    else
                    {
                        COREWEBVIEW2_WEB_ERROR_STATUS status;
                        args->get_WebErrorStatus(&status);
                        CoTaskMemString uri;
                        webView->get_Source(&uri);
                        auto errorStr = "Navigation failed (status="
                                        + std::to_string(status) + ") for url="
                                        + uri.toString();
                        LOG("WebView2: " + errorStr);
                        owner.onNavigationFailed(errorStr);
                    }
                    return S_OK;
                })
                .Get(),
            &navigationCompletedToken);

        webView->add_DocumentTitleChanged(
            Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                [this](ICoreWebView2*, IUnknown*) -> HRESULT
                {
                    CoTaskMemString title;
                    webView->get_DocumentTitle(&title);
                    auto titleStr = title.toString();
                    Threads::callAsync([cb = owner.onTitleChanged, titleStr]()
                                       { cb(titleStr); });
                    return S_OK;
                })
                .Get(),
            &titleChangedToken);

        webView->add_WebMessageReceived(
            Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                {
                    CoTaskMemString messageRaw;
                    args->get_WebMessageAsJson(&messageRaw);
                    if (!messageRaw)
                        return S_OK;

                    auto json = messageRaw.toString();
                    auto name = extractJsonStringField(json, "name");
                    auto body = extractJsonStringField(json, "body");

                    auto it = messageHandlers.find(name);
                    if (it == messageHandlers.end())
                        return S_OK;

                    // Invoke directly on the UI thread (where this
                    // callback already runs). Going through callAsync
                    // would post onto the dispatcher queue, which on
                    // Windows doesn't get drained while a nested
                    // runEventLoopFor is pumping — so bridge messages
                    // would never reach the handler and tests that
                    // depend on bridge round-trips would silently see
                    // no data.
                    it->second(body);
                    return S_OK;
                })
                .Get(),
            &webMessageToken);
    }

    static std::string extractJsonStringField(const std::string& json,
                                              const std::string& key)
    {
        auto needle = "\"" + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos)
            return {};

        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return {};

        auto start = json.find('"', pos);
        if (start == std::string::npos)
            return {};
        ++start;

        auto out = std::string();
        for (auto i = start; i < json.size(); ++i)
        {
            auto c = json[i];
            if (c == '\\' && i + 1 < json.size())
            {
                out.push_back(json[i + 1]);
                ++i;
                continue;
            }
            if (c == '"')
                return out;
            out.push_back(c);
        }
        return {};
    }

    void updateBounds()
    {
        if (!childHwnd)
            return;

        auto globalBounds = calculateGlobalBounds();
        auto dpiScale = getDpiScale();

        auto x = static_cast<int>(globalBounds.x * dpiScale);
        auto y = static_cast<int>(globalBounds.y * dpiScale);
        auto w = static_cast<int>(globalBounds.w * dpiScale);
        auto h = static_cast<int>(globalBounds.h * dpiScale);

        SetWindowPos(childHwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

        if (controller)
        {
            RECT bounds = {0, 0, w, h};
            controller->put_Bounds(bounds);
        }
    }

    Rect calculateGlobalBounds()
    {
        Rect globalBounds = owner.getLocalBounds();
        View* current = owner.getParent();
        float offsetX = 0;
        float offsetY = 0;

        while (current)
        {
            offsetX += current->getBounds().x;
            offsetY += current->getBounds().y;
            current = current->getParent();
        }

        globalBounds.x = offsetX;
        globalBounds.y = offsetY;

        return globalBounds;
    }

    float getDpiScale()
    {
        if (childHwnd)
        {
            auto dpi = GetDpiForWindow(childHwnd);
            return static_cast<float>(dpi) / 96.f;
        }
        return 1.f;
    }

    void queueOperation(std::function<void()> op)
    {
        if (initialized)
        {
            op();
        }
        else
        {
            pendingOperations.push(std::move(op));
        }
    }

    // Document-start scripts have to be registered with WebView2 *before*
    // Navigate(), otherwise they won't fire for the first document load.
    // Keep them in their own bucket so processPendingOperations() can
    // drain them ahead of the queued Navigate() — fixes the case where
    // the WebView constructor's auto-load enqueues a Navigate before
    // anyone has a chance to attach a user script.
    void queueDocStartScript(std::wstring script)
    {
        if (initialized && webView)
        {
            webView->AddScriptToExecuteOnDocumentCreated(script.c_str(), nullptr);
        }
        else
        {
            pendingDocStartScripts.push_back(std::move(script));
        }
    }

    void processPendingOperations()
    {
        for (auto& script: pendingDocStartScripts)
        {
            if (webView)
                webView->AddScriptToExecuteOnDocumentCreated(script.c_str(),
                                                             nullptr);
        }
        pendingDocStartScripts.clear();

        while (!pendingOperations.empty())
        {
            auto op = std::move(pendingOperations.front());
            pendingOperations.pop();
            op();
        }
    }

    WebView& owner;
    WebView::Options options;
    HWND childHwnd = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;
    MessageHandlerMap messageHandlers;
    std::unordered_map<std::string, ResourceProvider> schemeProviders;
    std::unordered_map<std::string, StreamingProvider> streamingProviders;

    bool initialized = false;
    bool initInProgress = false;
    std::queue<std::function<void()>> pendingOperations;
    std::vector<std::wstring> pendingDocStartScripts;

    EventRegistrationToken navigationStartingToken {};
    EventRegistrationToken navigationCompletedToken {};
    EventRegistrationToken titleChangedToken {};
    EventRegistrationToken webMessageToken {};
    EventRegistrationToken webResourceToken {};
};

void WebView::initNative(Options options)
{
    impl = std::make_shared<Native>(*this, std::move(options));
    registerWebView(this);
}

WebView::~WebView()
{
    // Close + childHwnd teardown happens in ~Native (driven by impl
    // going out of scope). Calling controller->Close() here too would
    // double-close, which the WebView2 docs say is a no-op but in
    // practice has been observed to trip late callbacks on Native.
    unregisterWebView(this);
}

void WebView::loadURL(const std::string& url)
{
    impl->ensureInitialized();
    impl->queueOperation(
        [this, url]()
        {
            if (impl->webView)
            {
                auto wideUrl = toWideString(url);
                impl->webView->Navigate(wideUrl.c_str());
            }
        });
}

void WebView::loadHTML(const std::string& html, const std::string& baseURL)
{
    impl->ensureInitialized();
    impl->queueOperation(
        [this, html, baseURL]()
        {
            if (impl->webView)
            {
                auto wideHtml = toWideString(html);
                impl->webView->NavigateToString(wideHtml.c_str());
            }
        });
}

void WebView::goBack()
{
    if (impl->webView)
    {
        impl->webView->GoBack();
    }
}

void WebView::goForward()
{
    if (impl->webView)
    {
        impl->webView->GoForward();
    }
}

void WebView::reload()
{
    if (impl->webView)
    {
        impl->webView->Reload();
    }
}

void WebView::stopLoading()
{
    if (impl->webView)
    {
        impl->webView->Stop();
    }
}

bool WebView::canGoBack() const
{
    if (!impl->webView)
        return false;

    BOOL canGoBack = FALSE;
    impl->webView->get_CanGoBack(&canGoBack);
    return canGoBack != FALSE;
}

bool WebView::canGoForward() const
{
    if (!impl->webView)
        return false;

    BOOL canGoForward = FALSE;
    impl->webView->get_CanGoForward(&canGoForward);
    return canGoForward != FALSE;
}

bool WebView::isLoading() const
{
    return false;
}

std::string WebView::getURL() const
{
    if (!impl->webView)
        return "";

    CoTaskMemString uri;
    impl->webView->get_Source(&uri);
    return uri.toString();
}

std::string WebView::getTitle() const
{
    if (!impl->webView)
        return "";

    CoTaskMemString title;
    impl->webView->get_DocumentTitle(&title);
    return title.toString();
}

void WebView::evaluateJavaScript(const std::string& script, JSCallback callback)
{
    if (!impl->webView)
    {
        if (callback)
        {
            callback("", "WebView not initialized");
        }
        return;
    }

    auto wideScript = toWideString(script);

    impl->webView->ExecuteScript(
        wideScript.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [callback](HRESULT errorCode, LPCWSTR resultJson) -> HRESULT
            {
                if (callback)
                {
                    std::string result;
                    std::string error;

                    if (FAILED(errorCode))
                    {
                        error = "Script execution failed";
                    }
                    else if (resultJson)
                    {
                        // WebView2 always returns JSON-encoded values, so a JS
                        // string like "abc" arrives as "\"abc\"". macOS hands
                        // back the raw string. Strip one JSON layer when the
                        // value is a string so both backends look the same to
                        // callers; numbers / bools / objects pass through.
                        result = unwrapJsonString(fromWideString(resultJson));
                    }

                    callback(result, error);
                }
                return S_OK;
            })
            .Get());
}

void WebView::takeSnapshot(SnapshotCallback callback)
{
    if (!callback)
        return;

    impl->ensureInitialized();
    impl->queueOperation(
        [this, callback]() mutable
        {
            if (!impl->webView)
            {
                Threads::callAsync([callback]
                                   { callback({}, "WebView not initialized"); });
                return;
            }

            ComPtr<IStream> stream;
            auto hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
            if (FAILED(hr) || !stream)
            {
                Threads::callAsync(
                    [callback] { callback({}, "CreateStreamOnHGlobal failed"); });
                return;
            }

            impl->webView->CapturePreview(
                COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
                stream.Get(),
                Microsoft::WRL::Callback<
                    ICoreWebView2CapturePreviewCompletedHandler>(
                    [callback, stream](HRESULT errorCode) -> HRESULT
                    {
                        std::vector<std::uint8_t> bytes;
                        std::string error;

                        if (FAILED(errorCode))
                        {
                            error = "CapturePreview failed";
                        }
                        else
                        {
                            // Stream is at end-of-write — rewind and
                            // read the full buffer back into bytes.
                            LARGE_INTEGER zero = {};
                            stream->Seek(zero, STREAM_SEEK_SET, nullptr);

                            STATSTG stat = {};
                            if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)))
                            {
                                bytes.resize(stat.cbSize.LowPart);
                                ULONG read = 0;
                                stream->Read(bytes.data(),
                                             static_cast<ULONG>(bytes.size()),
                                             &read);
                                bytes.resize(read);
                            }
                            else
                            {
                                error = "IStream::Stat failed";
                            }
                        }

                        Threads::callAsync(
                            [callback, bytes = std::move(bytes), error]() mutable
                            { callback(std::move(bytes), error); });
                        return S_OK;
                    })
                    .Get());
        });
}

void WebView::addScriptMessageHandler(
    const std::string& name, std::function<void(const std::string& message)> handler)
{
    impl->messageHandlers[name] = std::move(handler);

    // Expose the handler under both the plain `window.<name>` form and the
    // WebKit `window.webkit.messageHandlers.<name>` form. macOS only offers the
    // latter, so mirroring it here lets the same page code post messages on
    // both platforms without branching.
    auto script = toWideString(
        "(function(){var send=function(msg){window.chrome.webview.postMessage("
        "{name:'"
        + name
        + "',body:msg});};"
          "window."
        + name
        + "={postMessage:send};"
          "window.webkit=window.webkit||{};"
          "window.webkit.messageHandlers=window.webkit.messageHandlers||{};"
          "window.webkit.messageHandlers."
        + name + "={postMessage:send};})();");

    impl->ensureInitialized();
    impl->queueDocStartScript(std::move(script));
}

void WebView::removeScriptMessageHandler(const std::string& name)
{
    impl->messageHandlers.erase(name);
}

void WebView::addUserScript(const std::string& source, bool atDocumentStart)
{
    impl->ensureInitialized();

    if (atDocumentStart)
    {
        impl->queueDocStartScript(toWideString(source));
        return;
    }

    impl->queueOperation([this, source]() { evaluateJavaScript(source); });
}

void WebView::resized()
{
    View::resized();
    impl->ensureInitialized();
    impl->updateBounds();
}

void WebView::armFileDrag(const std::vector<std::string>&)
{
    // Native file drag-out is implemented on macOS only (WKWebView subclass +
    // NSDraggingSession started from a real mouseDragged: event). Not wired on
    // Windows yet; the assert marks it unimplemented and fails loudly if hit.
    assert(false && "armFileDrag is macOS-only");
}

void WebView::zoomIn()
{
    setZoom(getZoom() * zoomStep);
}

void WebView::zoomOut()
{
    setZoom(getZoom() / zoomStep);
}

void WebView::resetZoom()
{
    setZoom(1.0);
}

void WebView::setZoom(double level)
{
    auto clamped = std::clamp(level, minZoomLevel, maxZoomLevel);
    if (impl->controller)
        impl->controller->put_ZoomFactor(clamped);
}

double WebView::getZoom() const
{
    if (!impl->controller)
        return 1.0;

    double factor = 1.0;
    impl->controller->get_ZoomFactor(&factor);
    return factor;
}

WebView* WebView::focused()
{
    auto foreground = GetForegroundWindow();
    if (!foreground)
        return nullptr;

    for (auto* view: registeredWebViews())
    {
        if (!view->impl || !view->impl->childHwnd)
            continue;

        auto top = GetAncestor(view->impl->childHwnd, GA_ROOT);
        if (top == foreground)
            return view;
    }

    return nullptr;
}

} // namespace eacp::Graphics
