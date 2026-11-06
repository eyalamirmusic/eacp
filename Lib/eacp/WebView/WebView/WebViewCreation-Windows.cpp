#include "WebView-Windows.h"

#include <eacp/Graphics/DComp-Windows.h>
#include <eacp/Core/Utils/Format.h>
#include <eacp/Core/Utils/Strings.h>

#include <shlwapi.h>
#include <shlobj.h>
#include <WebView2EnvironmentOptions.h>

namespace eacp::Graphics
{

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
    GetModuleFileNameW(
        (HMODULE) eacp::Plugins::getCurrentModuleHandle(), modulePath, MAX_PATH);

    auto moduleName = std::wstring {PathFindFileNameW(modulePath)};
    if (auto dot = moduleName.rfind(L'.'); dot != std::wstring::npos)
        moduleName.resize(dot);
    if (moduleName.empty())
        moduleName = L"eacp";

    auto leaf = std::wstring {L"WebView2"};
    if (!suffix.empty())
        leaf += L"-" + Strings::widen(suffix);

    return folder + L"\\" + moduleName + L"\\" + leaf;
}
} // namespace

void WebView::Native::ensureInitialized()
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

void WebView::Native::createRenderVisual()
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

    if (auto* container = static_cast<IDCompositionVisual2*>(owner.getNativeLayer()))
        insertVisualAtTop(container, webViewVisual.Get());

    commitComposition();
}

void WebView::Native::createWebView2()
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
            { return handleEnvironmentCreated(result, env); })
            .Get());

    if (FAILED(hr))
    {
        LOG("WebView2: CreateCoreWebView2EnvironmentWithOptions failed hr=0x"
            + formatHresultAsHex(hr));
        initInProgress = false;
    }
}

HRESULT WebView::Native::handleEnvironmentCreated(HRESULT result,
                                                  ICoreWebView2Environment* env)
{
    if (FAILED(result) || !env)
    {
        LOG("WebView2: env create failed hr=0x" + formatHresultAsHex(result));
        return handleCreateFailure(result);
    }

    environment = env;
    return createCompositionController();
}

HRESULT WebView::Native::handleCreateFailure(HRESULT result)
{
    if (shouldRetryWebView2Create())
        return retryWebView2Create();

    initInProgress = false;
    return result;
}

// Headless CI machines intermittently lose the first WebView2 launch: the
// browser process dies mid-creation (E_ABORT), which takes the environment
// down with it. Asking that environment again is what the old retry did,
// and it always failed instantly — the process backing it is gone. So a
// retry has to launch a fresh environment instead. Popups have to keep the
// opener's environment (put_NewWindow requires it), and real sessions fail
// for real reasons (e.g. a missing runtime), so both surface the error.
bool WebView::Native::shouldRetryWebView2Create() const
{
    return Apps::getAppEnvironment().headless && !sharedEnvironment
           && webView2CreateRetries < maxWebView2CreateRetries;
}

HRESULT WebView::Native::retryWebView2Create()
{
    ++webView2CreateRetries;
    LOG("WebView2: retrying create with a fresh environment (",
        webView2CreateRetries,
        "/",
        maxWebView2CreateRetries,
        ")");

    environment.Reset();

    // Start the next attempt from the event loop rather than from inside
    // the failed handler, so WebView2 finishes unwinding this one first.
    // initInProgress stays set, which keeps ensureInitialized out until
    // the retry settles.
    Threads::callAsync(
        [this, guard = alive]
        {
            if (*guard)
                createWebView2();
        });

    return S_OK;
}

// Creates the visual-hosting composition controller from `environment` (set
// by either the normal env-creation path or popup mode) and wires the
// CoreWebView2 up. Shared by both so popups don't duplicate the setup.
HRESULT WebView::Native::createCompositionController()
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
            { return handleCompositionControllerCreated(result, ctrl); })
            .Get());
}

HRESULT WebView::Native::handleCompositionControllerCreated(
    HRESULT result, ICoreWebView2CompositionController* ctrl)
{
    if (FAILED(result) || !ctrl)
    {
        LOG("WebView2: composition controller create failed hr=0x"
            + formatHresultAsHex(result));
        return handleCreateFailure(result);
    }

    compositionController = ctrl;

    // The composition controller also implements the base
    // controller interface (bounds, settings, visibility, the
    // CoreWebView2 itself).
    if (FAILED(ctrl->QueryInterface(IID_PPV_ARGS(&controller))) || !controller)
    {
        LOG("WebView2: ICoreWebView2Controller QI failed");
        initInProgress = false;
        return E_NOINTERFACE;
    }

    finishInitialization();
    return S_OK;
}

void WebView::Native::finishInitialization()
{
    controller->get_CoreWebView2(&webView);
    applyBackground();

    // Render into our composition visual.
    compositionController->put_RootVisualTarget(webViewVisual.Get());

    applySettings();
    setupEventHandlers();
    registerSchemeHandlers();
    updateBounds();

    // A window hidden while the controller was still
    // initializing (a tray app's panel, hidden right after it
    // is constructed) must not have the browser's window come
    // up on screen now.
    applyControllerVisibility();

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
}

std::string formatHresultAsHex(HRESULT hr)
{
    return Strings::format("{:08x}", static_cast<unsigned long>(hr));
}

Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions>
    WebView::Native::buildEnvironmentOptions()
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
        auto wide = Strings::widen(scheme);
        auto registration =
            Microsoft::WRL::Make<CoreWebView2CustomSchemeRegistration>(wide.c_str());

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

void WebView::Native::applySettings()
{
    if (!webView)
        return;

    ComPtr<ICoreWebView2Settings> settings;
    webView->get_Settings(&settings);

    if (settings)
    {
        settings->put_AreDevToolsEnabled(options.debugConsole ? TRUE : FALSE);
        settings->put_AreDefaultContextMenusEnabled(
            options.defaultContextMenu ? TRUE : FALSE);
        settings->put_IsStatusBarEnabled(options.statusBar ? TRUE : FALSE);
        applyUserAgent(settings);
    }
}

// ICoreWebView2Settings2 is where the UA lives; an older runtime simply
// does not have it, and the view keeps the platform's own string.
void WebView::Native::applyUserAgent(const ComPtr<ICoreWebView2Settings>& settings)
{
    if (options.userAgent.empty())
        return;

    ComPtr<ICoreWebView2Settings2> settings2;

    if (SUCCEEDED(settings.As(&settings2)) && settings2)
        settings2->put_UserAgent(Strings::widen(options.userAgent).c_str());
}

void WebView::Native::applyBackground()
{
    if (!options.transparentBackground || !controller)
        return;

    ComPtr<ICoreWebView2Controller2> controller2;
    if (FAILED(controller.As(&controller2)) || !controller2)
        return;

    COREWEBVIEW2_COLOR clear = {};
    controller2->put_DefaultBackgroundColor(clear);
}
} // namespace eacp::Graphics
