#include <eacp/Core/Utils/WinInclude.h>

#include "WebView.h"
#include "StreamingRange.h"
#include "WebViewDetail.h"
#include <eacp/Graphics/Helpers/StringUtils-Windows.h>

#include <algorithm>
#include <eacp/Core/Utils/Containers.h>
#include <cassert>
#include <unordered_map>
#include <queue>
#include <functional>
#include <string>

#include <objbase.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <ole2.h>

#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <cmath>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Logging.h>

namespace wuc = winrt::Windows::UI::Composition;

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

HWND findHostHwndForView(View* view);

// Defined in Graphics/D2DFactory-Windows.cpp (linked via eacp-graphics).
wuc::Compositor getWinRTCompositor();

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
// WebView2's default user data folder sits next to the executable, which is
// read-only for installed apps (e.g. Program Files) and makes environment
// creation fail outright. Use %LOCALAPPDATA%\<exe name>\WebView2 instead;
// WebView2 creates the directories on demand. A non-empty `suffix`
// (Options::userDataFolderSuffix) isolates a WebView into its own folder
// (WebView2-<suffix>) so views with different custom-scheme registrations in
// the same executable don't collide on a shared folder — see the option's doc.
std::wstring defaultUserDataFolder(const std::string& suffix)
{
    PWSTR localAppData = nullptr;
    if (FAILED(
            SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
        return {};

    auto folder = std::wstring {localAppData};
    CoTaskMemFree(localAppData);

    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

    auto exeName = std::wstring {PathFindFileNameW(modulePath)};
    if (auto dot = exeName.rfind(L'.'); dot != std::wstring::npos)
        exeName.resize(dot);
    if (exeName.empty())
        exeName = L"eacp";

    auto leaf = std::wstring {L"WebView2"};
    if (!suffix.empty())
        leaf += L"-" + toWideString(suffix);

    return folder + L"\\" + exeName + L"\\" + leaf;
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

// Carries what a window.open popup needs to splice into WebView2's
// NewWindowRequested flow: the opener's environment (the adopted CoreWebView2
// must share it), the opener's options (so the popup serves the same schemes),
// and the callback that hands our CoreWebView2 back once it is live.
struct WebView::PopupInit
{
    ComPtr<ICoreWebView2Environment> environment;
    WebView::Options options;
    std::function<void(ICoreWebView2*)> onReady;
};

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
            if (permissionRequestedToken.value)
                webView->remove_PermissionRequested(permissionRequestedToken);
            if (newWindowRequestedToken.value)
                webView->remove_NewWindowRequested(newWindowRequestedToken);
            if (windowCloseRequestedToken.value)
                webView->remove_WindowCloseRequested(windowCloseRequestedToken);
        }

        if (controller)
            controller->Close();

        // Drop our COM references so the WebView2 browser-process IPC threads
        // release their connection while the heap is still valid. The shared
        // environment (popup mode) is owned by the opener; just drop our ref.
        webView.Reset();
        compositionController.Reset();
        controller.Reset();
        environment.Reset();
        sharedEnvironment.Reset();

        // Remove the render visual from the View's composition tree.
        if (webViewVisual)
        {
            if (auto* container =
                    static_cast<wuc::ContainerVisual*>(owner.getNativeLayer()))
                if (*container)
                    (*container).Children().Remove(webViewVisual);

            webViewVisual = nullptr;
        }
    }

    void ensureInitialized()
    {
        if (initialized || initInProgress)
            return;

        hostHwnd = findHostHwndForView(&owner);
        if (!hostHwnd)
            return;

        // In visual hosting mode WebView2 is not an HWND, so it receives no
        // native input. Route the framework's mouse events to it instead.
        owner.setHandlesMouseEvents(true);

        createRenderVisual();

        initInProgress = true;
        createWebView2();
    }

    void createRenderVisual()
    {
        if (webViewVisual)
            return;

        auto compositor = getWinRTCompositor();
        if (!compositor)
            return;

        webViewVisual = compositor.CreateContainerVisual();

        if (auto* container =
                static_cast<wuc::ContainerVisual*>(owner.getNativeLayer()))
            if (*container)
                (*container).Children().InsertAtTop(webViewVisual);
    }

    void createWebView2()
    {
        if (!hostHwnd || !webViewVisual)
            return;

        // Popup mode reuses the opener's environment (put_NewWindow requires the
        // adopted CoreWebView2 to share the opener's environment), so skip
        // environment creation and go straight to the controller.
        if (sharedEnvironment)
        {
            environment = sharedEnvironment;
            createCompositionController();
            return;
        }

        auto envOptions = buildEnvironmentOptions();
        auto userDataFolder = defaultUserDataFolder(options.userDataFolderSuffix);

        auto hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            userDataFolder.empty() ? nullptr : userDataFolder.c_str(),
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
                    return createCompositionController();
                })
                .Get());

        if (FAILED(hr))
        {
            LOG("WebView2: CreateCoreWebView2EnvironmentWithOptions failed hr=0x"
                + hresultHex(hr));
            initInProgress = false;
        }
    }

    // Headless CI machines intermittently abort composition controller
    // creation (E_ABORT) even though an immediate retry succeeds. Real
    // sessions fail for real reasons (e.g. a missing runtime) and should
    // surface the error right away.
    bool shouldRetryCompositionControllerCreate() const
    {
        return Apps::getAppEnvironment().headless
               && compositionCreateRetries < maxCompositionCreateRetries;
    }

    HRESULT retryCompositionControllerCreate()
    {
        ++compositionCreateRetries;
        LOG("WebView2: retrying composition controller create ("
            + std::to_string(compositionCreateRetries) + "/"
            + std::to_string(maxCompositionCreateRetries) + ")");
        return createCompositionController();
    }

    // Creates the visual-hosting composition controller from `environment` (set
    // by either the normal env-creation path or popup mode) and wires the
    // CoreWebView2 up. Shared by both so popups don't duplicate the setup.
    HRESULT createCompositionController()
    {
        // Visual hosting needs the composition-controller factory on
        // ICoreWebView2Environment3.
        ComPtr<ICoreWebView2Environment3> env3;
        if (FAILED(environment->QueryInterface(IID_PPV_ARGS(&env3))) || !env3)
        {
            LOG("WebView2: ICoreWebView2Environment3 unavailable; "
                "visual hosting requires a newer WebView2 runtime");
            initInProgress = false;
            return E_NOINTERFACE;
        }

        return env3->CreateCoreWebView2CompositionController(
            hostHwnd,
            Microsoft::WRL::Callback<
                ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                [this](HRESULT result,
                       ICoreWebView2CompositionController* ctrl) -> HRESULT
                {
                    if (FAILED(result) || !ctrl)
                    {
                        LOG("WebView2: composition controller create failed hr=0x"
                            + hresultHex(result));

                        if (shouldRetryCompositionControllerCreate())
                            return retryCompositionControllerCreate();

                        initInProgress = false;
                        return result;
                    }

                    compositionController = ctrl;

                    // The composition controller also implements the base
                    // controller interface (bounds, settings, visibility, the
                    // CoreWebView2 itself).
                    if (FAILED(ctrl->QueryInterface(IID_PPV_ARGS(&controller)))
                        || !controller)
                    {
                        LOG("WebView2: ICoreWebView2Controller QI failed");
                        initInProgress = false;
                        return E_NOINTERFACE;
                    }

                    controller->get_CoreWebView2(&webView);

                    // Render into our composition visual.
                    compositionController->put_RootVisualTarget(
                        reinterpret_cast<IUnknown*>(winrt::get_abi(webViewVisual)));

                    applySettings();
                    setupEventHandlers();
                    registerSchemeHandlers();
                    updateBounds();

                    controller->put_IsVisible(TRUE);

                    initialized = true;
                    initInProgress = false;

                    processPendingOperations();

                    // Popup mode: hand our freshly-created (un-navigated)
                    // CoreWebView2 back to the opener so it can adopt it via
                    // put_NewWindow before WebView2 navigates window.open's
                    // target into it.
                    if (onCoreWebViewReady)
                    {
                        auto ready = std::move(onCoreWebViewReady);
                        onCoreWebViewReady = nullptr;
                        ready(webView.Get());
                    }

                    return S_OK;
                })
                .Get());
    }

    static std::string hresultHex(HRESULT hr)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08lx", static_cast<unsigned long>(hr));
        return buf;
    }

    Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> buildEnvironmentOptions()
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
            Vector<Microsoft::WRL::ComPtr<ICoreWebView2CustomSchemeRegistration>> {};
        registrations.reserveAtLeast(
            (int) (options.schemes.size() + options.streamingSchemes.size()));

        auto addRegistration = [&](const std::string& scheme)
        {
            auto wide = toWideString(scheme);
            auto registration =
                Microsoft::WRL::Make<CoreWebView2CustomSchemeRegistration>(
                    wide.c_str());

            registration->put_TreatAsSecure(TRUE);
            registration->put_HasAuthorityComponent(TRUE);

            registrations.add(registration);
        };

        for (auto& [scheme, _]: options.schemes)
            addRegistration(scheme);
        for (auto& [scheme, _]: options.streamingSchemes)
            addRegistration(scheme);

        if (!registrations.empty())
        {
            auto raw =
                Vector<ICoreWebView2CustomSchemeRegistration*>(registrations.size());
            for (auto i = 0; i < registrations.size(); ++i)
                raw[i] = registrations[i].Get();

            Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions4> opts4;
            if (SUCCEEDED(envOptions.As(&opts4)) && opts4)
            {
                opts4->SetCustomSchemeRegistrations(static_cast<UINT32>(raw.size()),
                                                    raw.data());
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

        ensureWebResourceHandler();

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

    // Attach the WebResourceRequested handler once. Both custom-scheme serving
    // and loadHTML's base-URL interception route through it, so it must exist
    // even when no custom schemes were registered.
    void ensureWebResourceHandler()
    {
        if (webResourceToken.value || !webView)
            return;

        webView->add_WebResourceRequested(
            Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT
                { return handleWebResourceRequested(args); })
                .Get(),
            &webResourceToken);
    }

    // Serves an in-memory HTML document for a real (http/https) base URL so the
    // page gets that URL's origin -- and, for https, a secure context. macOS's
    // loadHTMLString:baseURL: gives the document a trustworthy origin directly;
    // WebView2's NavigateToString can't, leaving the page at about:blank where
    // secure-context-only APIs (e.g. navigator.mediaDevices) are unavailable.
    void serveInlineHtml(const std::string& baseURL, std::string html)
    {
        if (!webView)
            return;

        inlineDocuments[baseURL] = std::move(html);

        ensureWebResourceHandler();

        auto pattern = toWideString(baseURL);
        auto hr = webView->AddWebResourceRequestedFilter(
            pattern.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        if (FAILED(hr))
            LOG("WebView2: AddWebResourceRequestedFilter('" + baseURL
                + "') failed hr=0x" + hresultHex(hr));

        webView->Navigate(pattern.c_str());
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

        if (auto inlineIt = inlineDocuments.find(url);
            inlineIt != inlineDocuments.end())
            return serveInlineDocument(args, inlineIt->second);

        auto schemeEnd = url.find("://");
        if (schemeEnd == std::string::npos)
            return S_OK;

        auto scheme = url.substr(0, schemeEnd);

        if (auto streamIt = streamingProviders.find(scheme);
            streamIt != streamingProviders.end() && streamIt->second)
            return handleStreamingRequest(
                args, request.Get(), url, streamIt->second);

        auto it = schemeProviders.find(scheme);
        if (it == schemeProviders.end() || !it->second)
            return S_OK;

        auto response = it->second(url);
        Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> webResponse;

        if (!response)
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

        Microsoft::WRL::ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(response->data.data(),
                                        static_cast<UINT>(response->data.size())));
        if (!stream)
        {
            environment->CreateWebResourceResponse(
                nullptr,
                500,
                L"Internal Server Error",
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

        if (SUCCEEDED(environment->CreateWebResourceResponse(stream.Get(),
                                                             response->statusCode,
                                                             reason,
                                                             headers.c_str(),
                                                             &webResponse)))
        {
            args->put_Response(webResponse.Get());
        }

        return S_OK;
    }

    HRESULT serveInlineDocument(ICoreWebView2WebResourceRequestedEventArgs* args,
                                const std::string& html)
    {
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(html.data()),
                                        static_cast<UINT>(html.size())));

        ComPtr<ICoreWebView2WebResourceResponse> webResponse;
        if (stream
            && SUCCEEDED(environment->CreateWebResourceResponse(
                stream.Get(),
                200,
                L"OK",
                L"Content-Type: text/html; charset=utf-8",
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

        auto plan =
            planStreamingResponse(readRequestHeader(request, L"Range"), *resource);

        auto headers = std::wstring {};
        for (const auto& [name, value]: plan.headers)
        {
            if (!headers.empty())
                headers += L"\r\n";
            headers += toWideString(name) + L": " + toWideString(value);
        }

        ComPtr<IStream> body;
        if (plan.hasBody)
            body = Microsoft::WRL::Make<ReaderStream>(
                std::move(*resource), plan.served.start, plan.served.length);

        if (SUCCEEDED(
                environment->CreateWebResourceResponse(body.Get(),
                                                       plan.statusCode,
                                                       statusReason(plan.statusCode),
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

    void evaluateScript(const std::string& script,
                        const WebView::JSCallback& callback)
    {
        if (!webView)
        {
            if (callback)
                callback("", "WebView not initialized");
            return;
        }

        auto wideScript = toWideString(script);

        webView->ExecuteScript(
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
                            // WebView2 always returns JSON-encoded values, so a
                            // JS string like "abc" arrives as "\"abc\"". macOS
                            // hands back the raw string. Decode one JSON layer
                            // when the value is a string so both backends look
                            // the same to callers; numbers / bools / objects
                            // pass through.
                            auto rawJson = fromWideString(resultJson);
                            try
                            {
                                auto value = Miro::Json::parse(rawJson);
                                result =
                                    value.isString() ? value.asString() : rawJson;
                            }
                            catch (const Miro::Json::ParseError&)
                            {
                                result = rawJson;
                            }
                        }

                        callback(result, error);
                    }
                    return S_OK;
                })
                .Get());
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
                    loading = true;
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
                    loading = false;
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
                        auto errorStr =
                            "Navigation failed (status=" + std::to_string(status)
                            + ") for url=" + uri.toString();
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

                    // The envelope is {"name": "...", "body": "..."} where body
                    // is itself a JSON-encoded string handed through unchanged.
                    auto name = std::string {};
                    auto body = std::string {};
                    try
                    {
                        auto envelope = Miro::Json::parse(messageRaw.toString());
                        if (envelope.isObject())
                        {
                            const auto& obj = envelope.asObject();
                            if (auto* field = Miro::Json::find(obj, "name");
                                field && field->isString())
                                name = field->asString();
                            if (auto* field = Miro::Json::find(obj, "body");
                                field && field->isString())
                                body = field->asString();
                        }
                    }
                    catch (const Miro::Json::ParseError&)
                    {
                        return S_OK;
                    }

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

        // Auto-grant camera / microphone the way the macOS backend does via its
        // requestMediaCapturePermissionForOrigin UIDelegate. Without a handler
        // WebView2 leaves the request in its default state, so getUserMedia from
        // a page loaded here would never be allowed. Other permission kinds fall
        // through to WebView2's default (prompt) behaviour.
        webView->add_PermissionRequested(
            Microsoft::WRL::Callback<ICoreWebView2PermissionRequestedEventHandler>(
                [](ICoreWebView2*,
                   ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT
                {
                    auto kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
                    args->get_PermissionKind(&kind);

                    if (kind == COREWEBVIEW2_PERMISSION_KIND_CAMERA
                        || kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE)
                        args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);

                    return S_OK;
                })
                .Get(),
            &permissionRequestedToken);

        // window.open / target="_blank" -> hand the embedder a new app-owned
        // WebView, mirroring the macOS createWebViewWithConfiguration delegate.
        webView->add_NewWindowRequested(
            Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
                { return handleNewWindowRequested(args); })
                .Get(),
            &newWindowRequestedToken);

        // JS window.close() -> notify the embedder (e.g. so it tears the popup
        // window down). Matches the macOS webViewDidClose delegate.
        webView->add_WindowCloseRequested(
            Microsoft::WRL::Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
                [this](ICoreWebView2*, IUnknown*) -> HRESULT
                {
                    Threads::callAsync([cb = owner.onClose]() { cb(); });
                    return S_OK;
                })
                .Get(),
            &windowCloseRequestedToken);
    }

    // Adopts WebView2's window.open into an app-owned WebView so window.opener /
    // postMessage between the two keep working. WebView2's contract: hand it an
    // un-navigated CoreWebView2 created from this same environment via
    // put_NewWindow, completing the event's deferral once that popup exists.
    // Our WebView builds its CoreWebView2 lazily (once the embedder parents it
    // into a window), so we defer and complete from the popup's ready callback.
    HRESULT
    handleNewWindowRequested(ICoreWebView2NewWindowRequestedEventArgs* args)
    {
        CoTaskMemString uri;
        args->get_Uri(&uri);
        auto url = uri.toString();

        ComPtr<ICoreWebView2Deferral> deferral;
        if (FAILED(args->GetDeferral(&deferral)) || !deferral)
            return S_OK;

        ComPtr<ICoreWebView2NewWindowRequestedEventArgs> argsHold {args};

        auto init = WebView::PopupInit {};
        init.environment = environment;
        init.options = options;
        // WebView2 navigates the popup to the requested URL itself, so the popup
        // must not auto-load its embedded index over it.
        init.options.embedded.autoLoad = false;
        init.onReady = [argsHold, deferral](ICoreWebView2* popupWebView)
        {
            argsHold->put_NewWindow(popupWebView);
            argsHold->put_Handled(TRUE);
            deferral->Complete();
        };

        auto popup = OwningPointer<WebView> {new WebView {std::move(init)}};

        if (!owner.onNewWindowRequested(std::move(popup), url))
        {
            // Embedder declined and let the popup be destroyed, so onReady will
            // never fire. Hand the request back to WebView2's default handling.
            args->put_Handled(FALSE);
            deferral->Complete();
        }

        return S_OK;
    }

    void updateBounds()
    {
        if (!webViewVisual)
            return;

        auto bounds = owner.getLocalBounds();
        auto dpiScale = getDpiScale();

        auto widthPx = bounds.w * dpiScale;
        auto heightPx = bounds.h * dpiScale;

        // The render visual lives inside the WebView's own View container, which
        // the View tree already positions and (via the Panel) applies opacity
        // to. So it only needs to sit at the container origin and be sized.
        //
        // WebView2 treats the RootVisualTarget's coordinate space as physical
        // pixels, but our composition root is already DPI-scaled. Counter-scale
        // the visual by 1/dpi and size it in pixels, so the browser's
        // pixel-space content maps back to the correct logical size on screen
        // (without this it renders dpi-times too large on high-DPI displays).
        webViewVisual.Offset({0.0f, 0.0f, 0.0f});
        webViewVisual.Scale({1.0f / dpiScale, 1.0f / dpiScale, 1.0f});
        webViewVisual.Size({widthPx, heightPx});

        if (!controller)
            return;

        // Bounds are in raw pixels (the default bounds mode); RasterizationScale
        // tells WebView2 the DPI so the page lays out at the right CSS size and
        // renders crisply. Needs ICoreWebView2Controller3; without it the
        // WebView still works (just softer).
        ComPtr<ICoreWebView2Controller3> controller3;
        if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller3)))
            && controller3)
            controller3->put_RasterizationScale(dpiScale);

        RECT rect = {0,
                     0,
                     static_cast<LONG>(std::lround(widthPx)),
                     static_cast<LONG>(std::lround(heightPx))};
        controller->put_Bounds(rect);
    }

    float getDpiScale()
    {
        if (hostHwnd)
            return static_cast<float>(GetDpiForWindow(hostHwnd)) / 96.f;
        return 1.f;
    }

    // --- Input forwarding ---------------------------------------------------
    // A visual-hosted WebView2 has no input HWND, so the framework forwards the
    // mouse events it routed to this View (see the WebView::mouse* overrides).
    void sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND kind,
                   const Point& localPos,
                   uint32_t mouseData = 0,
                   COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS virtualKeys =
                       COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE)
    {
        if (!compositionController)
            return;

        // Bounds are in physical pixels (see updateBounds), so input points must
        // be too. The framework hands us positions local to this View in logical
        // units, so scale them up by the DPI factor.
        auto scale = getDpiScale();
        POINT pt = {static_cast<LONG>(std::lround(localPos.x * scale)),
                    static_cast<LONG>(std::lround(localPos.y * scale))};

        compositionController->SendMouseInput(kind, virtualKeys, mouseData, pt);
    }

    // The button held during a drag. WebView2 treats a MOVE whose virtualKeys
    // report no button as a plain hover, so without this a scrollbar-thumb (or
    // any in-page) drag is dropped the moment the pointer moves.
    static COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS
        heldButtonFor(const MouseEvent& event)
    {
        if (event.type != MouseEventType::Dragged)
            return COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
        if (event.button == MouseButton::Right)
            return COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
        if (event.button == MouseButton::Middle)
            return COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
        return COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
    }

    static COREWEBVIEW2_MOUSE_EVENT_KIND downKindFor(MouseButton button)
    {
        if (button == MouseButton::Right)
            return COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
        if (button == MouseButton::Middle)
            return COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
        return COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
    }

    static COREWEBVIEW2_MOUSE_EVENT_KIND upKindFor(MouseButton button)
    {
        if (button == MouseButton::Right)
            return COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
        if (button == MouseButton::Middle)
            return COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
        return COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
    }

    void handleMouseDown(const MouseEvent& event)
    {
        resetDragArming();

        if (controller)
            controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);

        sendMouse(downKindFor(event.button), event.pos);
    }

    void handleMouseUp(const MouseEvent& event)
    {
        sendMouse(upKindFor(event.button), event.pos);
    }

    void handleMouseMove(const MouseEvent& event)
    {
        sendMouse(
            COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE, event.pos, 0, heldButtonFor(event));
    }

    void handleMouseWheel(const MouseEvent& event)
    {
        // event.delta is in WHEEL_DELTA units; WebView2 expects the same value
        // packed into mouseData. y drives the vertical wheel, x the horizontal.
        if (event.delta.y != 0.f)
            sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL,
                      event.pos,
                      static_cast<uint32_t>(static_cast<int32_t>(event.delta.y)));

        if (event.delta.x != 0.f)
            sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL,
                      event.pos,
                      static_cast<uint32_t>(static_cast<int32_t>(event.delta.x)));
    }

    // --- Native file drag-out -----------------------------------------------
    // The page arms a drag on mousedown (via the armFileDrag bridge command);
    // by the time the pointer starts dragging the armed paths have arrived over
    // the message channel, so the first mouseDragged after arming kicks off the
    // OS drag. Mirrors the macOS EacpDragWebView gesture.
    bool startArmedFileDragIfNeeded()
    {
        if (!dragArmed)
            return false;

        dragArmed = false;
        auto paths = std::move(armedDragPaths);
        armedDragPaths = {};
        performFileDrag(paths);
        return true;
    }

    void performFileDrag(const Vector<std::string>& paths)
    {
        if (paths.empty() || !hostHwnd)
            return;

        // DoDragDrop needs the thread OLE-initialized. The app inits COM as an
        // STA for WinRT, but OLE drag/drop wants OleInitialize specifically;
        // balance it with OleUninitialize once the modal drag returns.
        auto oleHr = OleInitialize(nullptr);

        auto pidls = Vector<PIDLIST_ABSOLUTE> {};

        for (auto& path: paths)
        {
            auto widePath = toWideString(path);
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(
                    SHParseDisplayName(widePath.c_str(), nullptr, &pidl, 0, nullptr))
                && pidl)
                pidls.add(pidl);
        }

        if (!pidls.empty())
        {
            ComPtr<IShellItemArray> items;
            if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(
                    static_cast<UINT>(pidls.size()),
                    const_cast<LPCITEMIDLIST*>(pidls.data()),
                    &items))
                && items)
            {
                ComPtr<IDataObject> dataObject;
                if (SUCCEEDED(items->BindToHandler(
                        nullptr, BHID_DataObject, IID_PPV_ARGS(&dataObject)))
                    && dataObject)
                {
                    // A null drop source lets the shell supply the default drag
                    // image and cursor; it also runs the modal drag loop and
                    // tracks the real (physically-down) mouse button, so the
                    // drag can escape to Explorer.
                    DWORD effect = DROPEFFECT_NONE;
                    SHDoDragDrop(hostHwnd,
                                 dataObject.Get(),
                                 nullptr,
                                 DROPEFFECT_COPY,
                                 &effect);
                }
            }
        }

        for (auto* pidl: pidls)
            CoTaskMemFree(pidl);

        if (SUCCEEDED(oleHr))
            OleUninitialize();
    }

    // --- Native window drag -------------------------------------------------
    // window-drag.js posts __eacpWindowDrag on a drag-region mousedown, which
    // arms this; the next mouseDragged hands the gesture to the OS. Mirrors the
    // macOS performWindowDragWithEvent: path.
    bool startArmedWindowDragIfNeeded()
    {
        if (!windowDragArmed)
            return false;

        windowDragArmed = false;
        performWindowDrag();
        return true;
    }

    void performWindowDrag()
    {
        if (!hostHwnd)
            return;

        // The canonical borderless-window drag: drop the capture the host has
        // from the button press, then tell the window the user grabbed its
        // caption. DefWindowProc then runs a modal move loop that follows the
        // still-down mouse until release.
        ReleaseCapture();
        SendMessageW(hostHwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }

    // Clear any arming left from a previous gesture, before the page sees this
    // mousedown and (asynchronously) re-arms for the new one. Without this a
    // click that armed but never dragged would leak its arm into a later drag.
    void resetDragArming()
    {
        dragArmed = false;
        armedDragPaths = {};
        windowDragArmed = false;
    }

    void handleMouseLeave()
    {
        sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, {0.0f, 0.0f});
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
            pendingDocStartScripts.add(std::move(script));
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
    HWND hostHwnd = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2CompositionController> compositionController;
    ComPtr<ICoreWebView2> webView;

    // The composition visual the browser renders into. It is a child of the
    // WebView's own View ContainerVisual, so it inherits the View tree's
    // position, opacity and z-order — letting the WebView layer, blend and
    // overlap with GPU and primitive content (impossible with a child HWND).
    wuc::ContainerVisual webViewVisual {nullptr};
    MessageHandlerMap messageHandlers;
    std::unordered_map<std::string, ResourceProvider> schemeProviders;
    std::unordered_map<std::string, StreamingProvider> streamingProviders;

    // base URL -> HTML body, for loadHTML() calls that carry a base URL (see
    // serveInlineHtml). Keyed by the exact URL the navigation requests.
    std::unordered_map<std::string, std::string> inlineDocuments;

    bool initialized = false;
    bool initInProgress = false;

    static constexpr int maxCompositionCreateRetries = 3;
    int compositionCreateRetries = 0;

    // Tracks navigation in flight (NavigationStarting -> Completed), so
    // isLoading() can answer like the macOS backend's webView.isLoading.
    bool loading = false;

    // Popup mode (set via PopupInit when adopting a window.open): reuse the
    // opener's environment and, once our CoreWebView2 is live, hand it back to
    // the opener through onCoreWebViewReady so it can call put_NewWindow.
    ComPtr<ICoreWebView2Environment> sharedEnvironment;
    std::function<void(ICoreWebView2*)> onCoreWebViewReady;

    // Set by armFileDrag (the bridge command); consumed by the next
    // mouseDragged, which starts the OS drag for these paths.
    bool dragArmed = false;
    Vector<std::string> armedDragPaths;

    // Set by armWindowDrag (the __eacpWindowDrag bridge command); consumed by
    // the next mouseDragged, which hands the gesture to the OS move loop.
    bool windowDragArmed = false;

    std::queue<std::function<void()>> pendingOperations;
    Vector<std::wstring> pendingDocStartScripts;

    EventRegistrationToken navigationStartingToken {};
    EventRegistrationToken navigationCompletedToken {};
    EventRegistrationToken titleChangedToken {};
    EventRegistrationToken webMessageToken {};
    EventRegistrationToken webResourceToken {};
    EventRegistrationToken permissionRequestedToken {};
    EventRegistrationToken newWindowRequestedToken {};
    EventRegistrationToken windowCloseRequestedToken {};
};

void WebView::initNative(Options options)
{
    impl = std::make_shared<Native>(*this, std::move(options));
    detail::registerWebView(this);
    installWindowDragSupport();
    installWindowControlSupport();
}

// Popup constructor (window.open). Builds the Native in popup mode: it adopts
// the opener's environment and fires init.onReady once its CoreWebView2 is live,
// so the opener can hand it to WebView2 via put_NewWindow. Created by the
// opener's NewWindowRequested handler; the embedder receives it through
// onNewWindowRequested and parents it into a window, which drives the lazy init.
WebView::WebView(PopupInit init)
{
    impl = std::make_shared<Native>(*this, std::move(init.options));
    impl->sharedEnvironment = std::move(init.environment);
    impl->onCoreWebViewReady = std::move(init.onReady);
    detail::registerWebView(this);
    installWindowDragSupport();
    installWindowControlSupport();
}

WebView::~WebView()
{
    // Controller Close + visual teardown happens in ~Native (driven by impl
    // going out of scope). Calling controller->Close() here too would
    // double-close, which the WebView2 docs say is a no-op but in
    // practice has been observed to trip late callbacks on Native.
    detail::unregisterWebView(this);
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
            if (!impl->webView)
                return;

            // A base URL gives the document a real origin (and, for https, a
            // secure context). NavigateToString can't carry one -- it lands the
            // page at about:blank -- so serve the HTML for the base URL through
            // an intercepted navigation instead. This matches macOS's
            // loadHTMLString:baseURL: and is what lets secure-context APIs such
            // as navigator.mediaDevices.getUserMedia work.
            if (!baseURL.empty())
            {
                impl->serveInlineHtml(baseURL, html);
                return;
            }

            auto wideHtml = toWideString(html);
            impl->webView->NavigateToString(wideHtml.c_str());
        });
}

// Like loadURL/loadHTML, the navigation commands queue until the lazily
// created CoreWebView2 exists, so e.g. loadURL() + reload() issued back to
// back both take effect instead of the latter being dropped.
void WebView::goBack()
{
    impl->ensureInitialized();
    impl->queueOperation(
        [this]
        {
            if (impl->webView)
                impl->webView->GoBack();
        });
}

void WebView::goForward()
{
    impl->ensureInitialized();
    impl->queueOperation(
        [this]
        {
            if (impl->webView)
                impl->webView->GoForward();
        });
}

void WebView::reload()
{
    impl->ensureInitialized();
    impl->queueOperation(
        [this]
        {
            if (impl->webView)
                impl->webView->Reload();
        });
}

void WebView::stopLoading()
{
    impl->ensureInitialized();
    impl->queueOperation(
        [this]
        {
            if (impl->webView)
                impl->webView->Stop();
        });
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
    return impl->loading;
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

// Queues like loadURL/loadHTML, so a script evaluated right after loadURL() on
// a not-yet-initialized WebView runs once the CoreWebView2 exists instead of
// being silently dropped.
void WebView::evaluateJavaScript(const std::string& script,
                                 const JSCallback& callback)
{
    impl->ensureInitialized();
    impl->queueOperation([this, script, callback]()
                         { impl->evaluateScript(script, callback); });
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
                        Bytes bytes;
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
                                bytes.resize((int) stat.cbSize.LowPart);
                                ULONG read = 0;
                                stream->Read(bytes.data(),
                                             static_cast<ULONG>(bytes.size()),
                                             &read);
                                bytes.resize((int) read);
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

// Visual hosting gives the WebView no input HWND of its own, and the key-focus
// routing that needs this is macOS-only, so the host view is the right target.
void* WebView::nativeFocusTarget()
{
    return View::nativeFocusTarget();
}

// In visual hosting mode the WebView is a composition visual with no input
// HWND, so the framework's routed mouse events are forwarded to the WebView2
// composition controller. (On macOS the native WKWebView receives input
// directly, so the equivalents there are no-ops.)
void WebView::mouseDown(const MouseEvent& event)
{
    impl->handleMouseDown(event);
}

void WebView::mouseUp(const MouseEvent& event)
{
    impl->handleMouseUp(event);
}

void WebView::mouseDragged(const MouseEvent& event)
{
    // A drag armed by the page takes over the gesture: starting it here, from
    // the genuine drag, is what lets a file drag escape to Explorer / a window
    // drag enter the OS move loop.
    if (impl->startArmedFileDragIfNeeded())
        return;

    if (impl->startArmedWindowDragIfNeeded())
        return;

    impl->handleMouseMove(event);
}

void WebView::mouseMoved(const MouseEvent& event)
{
    impl->handleMouseMove(event);
}

void WebView::mouseExited(const MouseEvent&)
{
    impl->handleMouseLeave();
}

void WebView::mouseWheel(const MouseEvent& event)
{
    impl->handleMouseWheel(event);
}

void WebView::armFileDrag(const Vector<std::string>& paths)
{
    // Defer the actual OS drag to the next mouseDragged (see
    // startArmedFileDragIfNeeded): a drag started straight from this async
    // bridge callback wouldn't be tied to the live mouse gesture.
    impl->armedDragPaths = paths;
    impl->dragArmed = true;
}

void WebView::armWindowDrag()
{
    // Defer to the next mouseDragged (see startArmedWindowDragIfNeeded): a move
    // loop started straight from this async bridge callback wouldn't be tied to
    // the live mouse gesture, and a mere click would start dragging the window.
    impl->windowDragArmed = true;
}

void WebView::performWindowControl(const std::string& action)
{
    auto root = impl->hostHwnd ? GetAncestor(impl->hostHwnd, GA_ROOT) : nullptr;
    if (!root)
        return;

    if (action == "minimize")
    {
        ShowWindow(root, SW_MINIMIZE);
        return;
    }

    if (action == "maximize")
    {
        ShowWindow(root, IsZoomed(root) ? SW_RESTORE : SW_MAXIMIZE);

        // Report the resulting state back so the page's data-eacp-maximized
        // attribute tracks reality instead of guessing.
        evaluateJavaScript(IsZoomed(root) ? "window.__eacpSetMaximized(true)"
                                          : "window.__eacpSetMaximized(false)");
        return;
    }

    if (action == "close")
        PostMessageW(root, WM_CLOSE, 0, 0);
}

void WebView::setZoom(double level)
{
    auto clamped = detail::clampZoom(level);
    impl->ensureInitialized();
    impl->queueOperation(
        [this, clamped]
        {
            if (impl->controller)
                impl->controller->put_ZoomFactor(clamped);
        });
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

    for (auto* view: detail::registeredWebViews())
    {
        if (!view->impl || !view->impl->hostHwnd)
            continue;

        // The host HWND is the top-level window the visual-hosted WebView lives
        // in; the focused WebView is the one whose window is in the foreground.
        if (view->impl->hostHwnd == foreground)
            return view;
    }

    return nullptr;
}

bool WebView::isRuntimeAvailable()
{
    auto version = CoTaskMemString();
    auto hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);

    return SUCCEEDED(hr) && version;
}

} // namespace eacp::Graphics
