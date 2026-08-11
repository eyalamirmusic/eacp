#include <eacp/Core/Utils/WinInclude.h>
#include <algorithm>

#include "WebView.h"
#include "StreamingRange.h"
#include "WebViewDetail.h"
#include "FileDrag-Windows.h"
#include <eacp/Graphics/DComp-Windows.h>
#include <eacp/Graphics/Helpers/StringUtils-Windows.h>
#include <eacp/Core/Threads/ThreadUtils.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <queue>

#include <objbase.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <ole2.h>

#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

HWND findHostHwndForView(View* view);

// Defined in Graphics/Keyboard-Windows.cpp; KeyCode::Unknown when unmapped.
uint16_t keyCodeFromVirtualKey(int vk);

using MessageHandlerMap =
    std::unordered_map<std::string, std::function<void(const std::string&)>>;

struct CoTaskMemString
{
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

    LPWSTR ptr = nullptr;
};

namespace
{
// WebView2 defaults to a folder next to the executable, which is read-only for
// installed apps and fails environment creation outright. See
// Options::userDataFolderSuffix for what the suffix isolates.
std::wstring defaultUserDataFolder(const std::string& suffix)
{
    PWSTR localAppData = nullptr;
    if (FAILED(
            SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
        return {};

    auto folder = std::wstring {localAppData};
    CoTaskMemFree(localAppData);

    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(
        (HMODULE) eacp::Plugins::getCurrentModuleHandle(), modulePath, MAX_PATH);

    auto moduleName = std::wstring {PathFindFileNameW(modulePath)};
    if (auto dot = moduleName.rfind(L'.'); dot != std::wstring::npos)
        moduleName.resize(dot);
    if (moduleName.empty())
        moduleName = L"eacp";

    auto leaf = std::wstring {L"WebView2"};
    if (!suffix.empty())
        leaf += L"-" + toWideString(suffix);

    return folder + L"\\" + moduleName + L"\\" + leaf;
}

// Read-only IStream over a byte range, pulled lazily as WebView2 reads the
// response body. Owns the resource, keeping its reader alive meanwhile.
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

// What a window.open popup needs to splice into NewWindowRequested. The
// adopted CoreWebView2 must share the opener's environment.
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
        *alive = false;

        // Unhook first, so a late callback from the browser process cannot
        // fire back into a half-destroyed Native.
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

        // Let the WebView2 IPC threads release their connection while this
        // heap is still valid.
        webView.Reset();
        compositionController.Reset();
        controller.Reset();
        environment.Reset();
        sharedEnvironment.Reset();

        if (webViewVisual)
        {
            if (auto* container =
                    static_cast<IDCompositionVisual2*>(owner.getNativeLayer()))
                container->RemoveVisual(webViewVisual.Get());

            webViewVisual.Reset();
            commitComposition();
        }
    }

    void ensureInitialized()
    {
        if (initialized || initInProgress)
            return;

        hostHwnd = findHostHwndForView(&owner);
        if (!hostHwnd)
            return;

        // A visual-hosted WebView2 is not an HWND and receives no native input.
        owner.setHandlesMouseEvents(true);

        createRenderVisual();

        initInProgress = true;
        createWebView2();
    }

    void createRenderVisual()
    {
        if (webViewVisual)
            return;

        auto* device = getCompositionDevice();
        if (!device)
            return;

        if (FAILED(device->CreateVisual(webViewVisual.GetAddressOf())))
        {
            webViewVisual.Reset();
            return;
        }

        if (auto* container =
                static_cast<IDCompositionVisual2*>(owner.getNativeLayer()))
            insertVisualAtTop(container, webViewVisual.Get());

        commitComposition();
    }

    void createWebView2()
    {
        if (!hostHwnd || !webViewVisual)
            return;

        // put_NewWindow requires a popup to share the opener's environment.
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

                        if (shouldRetryWebView2Create())
                            return retryWebView2Create();

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

    // On headless CI the browser process intermittently dies mid-creation,
    // taking its environment with it, so only a fresh environment can retry.
    // Popups must keep the opener's, and real sessions fail for real reasons.
    bool shouldRetryWebView2Create() const
    {
        return Apps::getAppEnvironment().headless && !sharedEnvironment
               && webView2CreateRetries < maxWebView2CreateRetries;
    }

    HRESULT retryWebView2Create()
    {
        ++webView2CreateRetries;
        LOG("WebView2: retrying create with a fresh environment (",
            webView2CreateRetries,
            "/",
            maxWebView2CreateRetries,
            ")");

        environment.Reset();

        // Retry from the event loop, so WebView2 unwinds this handler first.
        // initInProgress stays set, keeping ensureInitialized out meanwhile.
        Threads::callAsync(
            [this, guard = alive]
            {
                if (*guard)
                    createWebView2();
            });

        return S_OK;
    }

    HRESULT createCompositionController()
    {
        // Visual hosting needs the factory on ICoreWebView2Environment3.
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

                        if (shouldRetryWebView2Create())
                            return retryWebView2Create();

                        initInProgress = false;
                        return result;
                    }

                    compositionController = ctrl;

                    if (FAILED(ctrl->QueryInterface(IID_PPV_ARGS(&controller)))
                        || !controller)
                    {
                        LOG("WebView2: ICoreWebView2Controller QI failed");
                        initInProgress = false;
                        return E_NOINTERFACE;
                    }

                    controller->get_CoreWebView2(&webView);
                    applyBackground();

                    compositionController->put_RootVisualTarget(webViewVisual.Get());

                    applySettings();
                    setupEventHandlers();
                    registerSchemeHandlers();
                    updateBounds();

                    // A host hidden mid-initialization must not have the
                    // browser's window surface now.
                    setControllerVisible(hostVisible);

                    initialized = true;
                    initInProgress = false;

                    processPendingOperations();

                    // Popup mode: the opener adopts this un-navigated
                    // CoreWebView2 via put_NewWindow.
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
        // Custom schemes need this env-level registration to navigate at all,
        // plus the per-CoreWebView2 filter that serves the bytes. Secure with
        // an authority so app://local/index.html parses as React expects.
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

        // Stable owners for the tables the WebResourceRequested callback reads.
        for (auto& [scheme, provider]: options.schemes)
            schemeProviders.emplace(scheme, provider);
        for (auto& [scheme, provider]: options.streamingSchemes)
            streamingProviders.emplace(scheme, provider);

        ensureWebResourceHandler();

        // Only URLs matching a registered filter reach the handler at all.
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

    // Needed even without custom schemes: loadHTML's base-URL interception
    // routes through the same handler.
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

    // Gives an in-memory document the base URL's origin, and for https a secure
    // context. NavigateToString cannot: it leaves the page at about:blank.
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

        // WebView2 enforces cross-origin rules even on our own scheme, so
        // fetch() to a sibling URL fails without an allow-origin header.
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
            settings->put_IsStatusBarEnabled(options.statusBar ? TRUE : FALSE);
        }
    }

    void applyBackground()
    {
        if (!options.transparentBackground || !controller)
            return;

        ComPtr<ICoreWebView2Controller2> controller2;
        if (FAILED(controller.As(&controller2)) || !controller2)
            return;

        COREWEBVIEW2_COLOR clear = {};
        controller2->put_DefaultBackgroundColor(clear);
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
                            // WebView2 JSON-encodes results, so "abc" arrives
                            // as "\"abc\"". Strip one layer to match macOS.
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

                    // The envelope is {"name": ..., "body": ...}. Objects and
                    // arrays are re-serialised, and bare scalars dropped, the
                    // way macOS's isValidJSONObject guard does.
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
                            if (auto* field = Miro::Json::find(obj, "body"); field)
                            {
                                if (field->isString())
                                    body = field->asString();
                                else if (field->isObject() || field->isArray())
                                    body = Miro::Json::print(*field);
                            }
                        }
                    }
                    catch (const Miro::Json::ParseError&)
                    {
                        return S_OK;
                    }

                    auto it = messageHandlers.find(name);
                    if (it == messageHandlers.end())
                        return S_OK;

                    // Directly, on the UI thread this already runs on: the
                    // dispatcher queue callAsync posts to is not drained
                    // while a nested runEventLoopFor is pumping.
                    it->second(body);
                    return S_OK;
                })
                .Get(),
            &webMessageToken);

        // Auto-grant camera / microphone as the macOS backend does; without a
        // handler WebView2 never allows getUserMedia. Other kinds still prompt.
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

        webView->add_NewWindowRequested(
            Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
                { return handleNewWindowRequested(args); })
                .Get(),
            &newWindowRequestedToken);

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

    // WebView2 wants an un-navigated CoreWebView2 from this environment via
    // put_NewWindow, but ours is built lazily once the embedder parents it into
    // a window — hence the deferral, completed from the popup's ready callback.
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
        // WebView2 navigates the popup itself; an embedded index would race it.
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
            // The declined popup is destroyed, so onReady will never fire.
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

        // The View container already positions the visual, so it sits at the
        // origin. WebView2 treats RootVisualTarget space as physical pixels
        // while our composition root is DPI-scaled, hence the 1/dpi transform.
        webViewVisual->SetOffsetX(0.0f);
        webViewVisual->SetOffsetY(0.0f);

        if (dpiScale > 0.f)
            webViewVisual->SetTransform(
                D2D1::Matrix3x2F::Scale(1.0f / dpiScale, 1.0f / dpiScale));

        commitComposition();

        if (!controller)
            return;

        // Bounds are raw pixels, so RasterizationScale is what gets the page
        // laid out at the right CSS size. Without Controller3 it just looks
        // softer.
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

    // Visual hosting still leaves WebView2 its own top-level window for input,
    // IME and accessibility, placed in screen coordinates it only re-derives
    // when told — otherwise it stays where the host was at creation.
    void notifyParentWindowMoved()
    {
        if (controller)
            controller->NotifyParentWindowPositionChanged();
    }

    // That window is shown and hidden independently of the host, so a hidden
    // host would otherwise leave it on screen, hit-testable. Visibility is
    // remembered, as it can change while the controller still initializes.
    void setControllerVisible(bool visible)
    {
        hostVisible = visible;

        if (controller)
            controller->put_IsVisible(visible ? TRUE : FALSE);
    }

    // A visual-hosted WebView2 has no input HWND, so the framework forwards the
    // mouse events it routed to this View.
    void sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND kind,
                   const Point& localPos,
                   uint32_t mouseData = 0,
                   COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS virtualKeys =
                       COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE)
    {
        if (!compositionController)
            return;

        // Bounds are physical pixels (see updateBounds), but the framework
        // hands us logical units.
        auto scale = getDpiScale();
        POINT pt = {static_cast<LONG>(std::lround(localPos.x * scale)),
                    static_cast<LONG>(std::lround(localPos.y * scale))};

        compositionController->SendMouseInput(kind, virtualKeys, mouseData, pt);
    }

    // WebView2 reads a MOVE whose virtualKeys report no button as a hover, and
    // would drop an in-page drag the moment the pointer moves.
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

    void moveFocusToContent()
    {
        if (hostHwnd)
            SetFocus(hostHwnd);

        if (controller)
            controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }

    void handleMouseMove(const MouseEvent& event)
    {
        sendMouse(
            COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE, event.pos, 0, heldButtonFor(event));
    }

    void handleMouseWheel(const MouseEvent& event)
    {
        // event.delta is already in the WHEEL_DELTA units mouseData wants.
        if (event.delta.y != 0.f)
            sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL,
                      event.pos,
                      static_cast<uint32_t>(static_cast<int32_t>(event.delta.y)));

        if (event.delta.x != 0.f)
            sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL,
                      event.pos,
                      static_cast<uint32_t>(static_cast<int32_t>(event.delta.x)));
    }

    // The page arms a drag on mousedown, so the armed paths have arrived over
    // the message channel by the time the pointer starts dragging.
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

        auto ownerWindow = hostHwnd;
        auto dpiScale = getDpiScale();
        auto onDragStarted = owner.onFileDragStarted;
        auto onDragMoved = owner.onFileDragMoved;
        auto onDragEnded = owner.onFileDragEnded;
        auto oleHr = OleInitialize(nullptr);
        if (FAILED(oleHr))
            return;

        auto pidls = Vector<PIDLIST_ABSOLUTE> {};

        for (auto& path: paths)
        {
            auto widePath = toWideString(path);
            // SHParseDisplayName yields no PIDL for forward slashes, and the
            // drag then silently never starts.
            std::replace(widePath.begin(), widePath.end(), L'/', L'\\');
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
                    if (onDragStarted)
                        onDragStarted();

                    if (IsWindow(ownerWindow))
                    {
                        // Attach, not construct-from-raw, so the source keeps
                        // the single ref it is born with.
                        ComPtr<IDropSource> dropSource;
                        dropSource.Attach(new FileDragSource(
                            [ownerWindow, dpiScale]
                            {
                                return fileDragPointFromCursor(ownerWindow,
                                                               dpiScale);
                            },
                            onDragMoved));

                        // Purely for cursor feedback; see FileDragTarget. Fails
                        // harmlessly if something else owns the drop slot.
                        ComPtr<IDropTarget> dropTarget;
                        dropTarget.Attach(new FileDragTarget());
                        auto registered = SUCCEEDED(
                            RegisterDragDrop(ownerWindow, dropTarget.Get()));

                        DWORD effect = DROPEFFECT_NONE;
                        SHDoDragDrop(ownerWindow,
                                     dataObject.Get(),
                                     dropSource.Get(),
                                     DROPEFFECT_COPY,
                                     &effect);

                        if (registered)
                            RevokeDragDrop(ownerWindow);

                        // The modal loop is over; report the release point.
                        onDragEnded(fileDragPointFromCursor(ownerWindow, dpiScale));
                    }
                }
            }
        }

        for (auto* pidl: pidls)
            CoTaskMemFree(pidl);

        if (SUCCEEDED(oleHr))
            OleUninitialize();
    }

    // window-drag.js posts __eacpWindowDrag on a drag-region mousedown, which
    // arms this; the next mouseDragged hands the gesture to the OS.
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

        // The canonical borderless-window drag: drop the button-press capture,
        // then let DefWindowProc run its modal caption-move loop.
        ReleaseCapture();
        SendMessageW(hostHwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }

    // Otherwise a click that armed but never dragged leaks into a later drag.
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

    // Kept in their own bucket because they must reach WebView2 before the
    // queued Navigate(), or they miss the first document load.
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

    // A child of the View's ContainerVisual, so it inherits position, opacity
    // and z-order and can blend with other content — a child HWND could not.
    Microsoft::WRL::ComPtr<IDCompositionVisual2> webViewVisual;
    MessageHandlerMap messageHandlers;
    std::unordered_map<std::string, ResourceProvider> schemeProviders;
    std::unordered_map<std::string, StreamingProvider> streamingProviders;

    // Keyed by the exact base URL the navigation requests. See serveInlineHtml.
    std::unordered_map<std::string, std::string> inlineDocuments;

    bool initialized = false;
    bool initInProgress = false;

    // See setControllerVisible.
    bool hostVisible = true;

    static constexpr int maxWebView2CreateRetries = 3;
    int webView2CreateRetries = 0;

    // Cleared by the destructor, so a queued create retry can tell whether the
    // Native that scheduled it is still there.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    // Navigation in flight: NavigationStarting -> NavigationCompleted.
    bool loading = false;

    // Popup mode, set via PopupInit when adopting a window.open.
    ComPtr<ICoreWebView2Environment> sharedEnvironment;
    std::function<void(ICoreWebView2*)> onCoreWebViewReady;

    // Set by armFileDrag / armWindowDrag; consumed by the next mouseDragged.
    bool dragArmed = false;
    Vector<std::string> armedDragPaths;
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

    if (impl->options.forwardUnhandledKeys)
        installKeyEventSupport();

    if (impl->options.driveOffscreenAnimation)
        installOffscreenAnimationSupport();
}

// Popup constructor (window.open): adopts the opener's environment and fires
// init.onReady once its CoreWebView2 is live. Init stays lazy until the
// embedder parents it into a window.
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
    // ~Native does the controller Close and visual teardown. Closing here too
    // is documented as a no-op, but has been seen to trip late callbacks.
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

            // NavigateToString carries no base URL and lands the page at
            // about:blank, so serve it through an intercepted navigation.
            if (!baseURL.empty())
            {
                impl->serveInlineHtml(baseURL, html);
                return;
            }

            auto wideHtml = toWideString(html);
            impl->webView->NavigateToString(wideHtml.c_str());
        });
}

// Queued until the lazily created CoreWebView2 exists, so loadURL() + reload()
// back to back both take effect.
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

// Queued, so a script evaluated on a not-yet-initialized WebView still runs.
void WebView::evaluateJavaScript(const std::string& script,
                                 const JSCallback& callback)
{
    Threads::assertMainThread();

    impl->ensureInitialized();
    impl->queueOperation([this, script, callback]()
                         { impl->evaluateScript(script, callback); });
}

void WebView::focusContent()
{
    focus();
    impl->ensureInitialized();
    impl->queueOperation([this] { impl->moveFocusToContent(); });
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
                            // The stream is at end-of-write; rewind to read.
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

    // Exposed under the WebKit `window.webkit.messageHandlers.<name>` form as
    // well, so page code posts messages the same way on both platforms.
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

void WebView::hostWindowMoved()
{
    impl->notifyParentWindowMoved();
}

void WebView::hostWindowVisibilityChanged(bool visible)
{
    impl->setControllerVisible(visible);
}

// The WebView has no input HWND of its own, and the routing that would need
// one is macOS-only.
void* WebView::nativeFocusTarget()
{
    return View::nativeFocusTarget();
}

// A composition visual receives no input of its own, so routed mouse events
// are forwarded to the WebView2 composition controller.
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
    // Only a drag started from the genuine gesture can escape the app.
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
    // Deferred to the next mouseDragged: a drag started from this async bridge
    // callback would not be tied to the live mouse gesture.
    impl->armedDragPaths = paths;
    impl->dragArmed = true;
}

void WebView::armWindowDrag()
{
    // Deferred to the next mouseDragged, or a mere click would start dragging.
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

        // Report the resulting state, so the page never has to guess it.
        evaluateJavaScript(IsZoomed(root) ? "window.__eacpSetMaximized(true)"
                                          : "window.__eacpSetMaximized(false)");
        return;
    }

    if (action == "close")
        PostMessageW(root, WM_CLOSE, 0, 0);
}

namespace
{
struct KeyVerdict
{
    bool isDown = false;
    bool consumed = false;
    int virtualKey = 0;
    ModifierKeys modifiers;
    bool isRepeat = false;
    std::string characters;
};

// Parses key-events.js's "<down|up>:<0|1>:<keyCode>:<mods>:<repeat>:<key>".
// event.key sits last so a literal ':' key cannot split the message.
bool parseKeyVerdict(const std::string& message, KeyVerdict& out)
{
    auto pos = std::size_t {0};

    auto next = [&]() -> std::string
    {
        if (pos == std::string::npos)
            return {};

        auto colon = message.find(':', pos);
        auto token = message.substr(
            pos, colon == std::string::npos ? std::string::npos : colon - pos);
        pos = colon == std::string::npos ? std::string::npos : colon + 1;
        return token;
    };

    auto kind = next();
    if (kind != "down" && kind != "up")
        return false;

    auto toInt = [](const std::string& field)
    { return field.empty() ? 0 : std::atoi(field.c_str()); };

    out.isDown = kind == "down";
    out.consumed = next() == "1";
    out.virtualKey = toInt(next());

    auto mods = toInt(next());
    out.modifiers = {
        (mods & 1) != 0, (mods & 2) != 0, (mods & 4) != 0, (mods & 8) != 0};

    out.isRepeat = next() == "1";

    // Only a single code point is typed text; named keys ("Enter", "ArrowUp")
    // are multi-char ASCII and carry no characters, as WM_CHAR would.
    auto key = pos == std::string::npos ? std::string {} : message.substr(pos);
    if (!key.empty()
        && (key.size() == 1 || static_cast<unsigned char>(key[0]) >= 0x80))
        out.characters = key;

    return true;
}

// Always a plain key message, never WM_SYSKEYDOWN, whose DefWindowProc would
// poke the window menu. Bit 30 of lParam marks an auto-repeat keydown.
void forwardKeyToHost(HWND host, const KeyVerdict& verdict)
{
    if (!host || verdict.virtualKey == 0)
        return;

    auto message = verdict.isDown ? UINT {WM_KEYDOWN} : UINT {WM_KEYUP};
    auto lParam = LPARAM {verdict.isDown && verdict.isRepeat ? 0x40000000 : 0};

    PostMessageW(host, message, static_cast<WPARAM>(verdict.virtualKey), lParam);
}
} // namespace

// WebView2's AcceleratorKeyPressed never fires for plain character keys, so the
// injected key-events.js is the only source of verdicts. Unconsumed keys go to
// onUnhandledKeyEvent, then back into the host window's WM_KEYDOWN path.
void WebView::installKeyEventSupport()
{
    auto shim = ResEmbed::get("key-events.js", "EacpWebView");
    if (!shim)
        throw std::runtime_error(
            "eacp-webview: embedded key-events.js resource not found");

    addUserScript(shim.toString(), true);

    addScriptMessageHandler(
        "__eacpKeyEvent",
        [this](const std::string& message)
        {
            auto verdict = KeyVerdict {};
            if (!parseKeyVerdict(message, verdict) || verdict.consumed)
                return;

            auto event = KeyEvent {};
            event.type = verdict.isDown ? KeyEventType::Down : KeyEventType::Up;
            event.keyCode = keyCodeFromVirtualKey(verdict.virtualKey);
            event.modifiers = verdict.modifiers;
            event.isRepeat = verdict.isRepeat;
            event.characters = verdict.characters;
            event.charactersIgnoringModifiers = verdict.characters;

            if (onUnhandledKeyEvent && onUnhandledKeyEvent(event))
                return;

            forwardKeyToHost(impl->hostHwnd, verdict);
        });
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
