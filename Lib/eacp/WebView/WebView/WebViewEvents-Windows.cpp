#include "WebView-Windows.h"

#include "WebViewMessages.h"
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Threads/ThreadUtils.h>

namespace eacp::Graphics
{

namespace
{
template <typename RemoveMethod>
void removeEventHandlerIfAdded(ICoreWebView2* view,
                               RemoveMethod removeMethod,
                               EventRegistrationToken token)
{
    if (token.value)
        (view->*removeMethod)(token);
}

// Adds a WebView2 event handler that hands the event's arguments to
// `handleArguments` and always answers S_OK.
template <typename Handler, typename AddMethod, typename HandleArguments>
void forwardEventToMember(ICoreWebView2* view,
                          AddMethod addMethod,
                          EventRegistrationToken& token,
                          HandleArguments handleArguments)
{
    (view->*addMethod)(Microsoft::WRL::Callback<Handler>(
                           [handleArguments](ICoreWebView2*, auto* args) -> HRESULT
                           {
                               handleArguments(args);
                               return S_OK;
                           })
                           .Get(),
                       &token);
}

bool isMediaCapturePermission(COREWEBVIEW2_PERMISSION_KIND kind)
{
    return kind == COREWEBVIEW2_PERMISSION_KIND_CAMERA
           || kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE;
}
} // namespace

// WebMessageReceived fires for the top-level document only, and Source is
// that document's URI. NavigateToString (loadHTML without a base URL) puts
// the page at about:blank.
bool WebView::Native::isFromTrustedSender(
    ICoreWebView2WebMessageReceivedEventArgs* args)
{
    ComAllocatedWideString sourceRaw;

    if (FAILED(args->get_Source(&sourceRaw)))
        return false;

    auto source = sourceRaw.toString();
    return originPolicy.admit(detail::originOf(source), source == "about:blank");
}

void WebView::Native::setupEventHandlers()
{
    if (!webView)
        return;

    addNavigationStartingHandler();
    addNavigationCompletedHandler();
    addDocumentTitleChangedHandler();
    addWebMessageReceivedHandler();

    // Auto-grant camera / microphone the way the macOS backend does via its
    // requestMediaCapturePermissionForOrigin UIDelegate. Other permission
    // kinds fall through to WebView2's default (prompt) behaviour, and so
    // do camera / microphone with Options::promptForMediaCapture.
    if (!options.promptForMediaCapture)
        addPermissionRequestedHandler();

    addNewWindowRequestedHandler();
    addWindowCloseRequestedHandler();
}

void WebView::Native::removeEventHandlers()
{
    if (!webView)
        return;

    auto* view = webView.Get();
    removeEventHandlerIfAdded(
        view, &ICoreWebView2::remove_NavigationStarting, navigationStartingToken);
    removeEventHandlerIfAdded(
        view, &ICoreWebView2::remove_NavigationCompleted, navigationCompletedToken);
    removeEventHandlerIfAdded(
        view, &ICoreWebView2::remove_DocumentTitleChanged, titleChangedToken);
    removeEventHandlerIfAdded(
        view, &ICoreWebView2::remove_WebMessageReceived, webMessageToken);
    removeEventHandlerIfAdded(
        view, &ICoreWebView2::remove_WebResourceRequested, webResourceToken);
    removeEventHandlerIfAdded(
        view, &ICoreWebView2::remove_PermissionRequested, permissionRequestedToken);
    removeEventHandlerIfAdded(
        view, &ICoreWebView2::remove_NewWindowRequested, newWindowRequestedToken);
    removeEventHandlerIfAdded(view,
                              &ICoreWebView2::remove_WindowCloseRequested,
                              windowCloseRequestedToken);
}

void WebView::Native::addNavigationStartingHandler()
{
    webView->add_NavigationStarting(
        Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT
            {
                loading = true;
                ComAllocatedWideString uri;
                args->get_Uri(&uri);
                auto url = uri.toString();
                Threads::callAsync([cb = owner.onNavigationStarted, url]()
                                   { cb(url); });
                return S_OK;
            })
            .Get(),
        &navigationStartingToken);
}

void WebView::Native::addNavigationCompletedHandler()
{
    forwardEventToMember<ICoreWebView2NavigationCompletedEventHandler>(
        webView.Get(),
        &ICoreWebView2::add_NavigationCompleted,
        navigationCompletedToken,
        [this](ICoreWebView2NavigationCompletedEventArgs* args)
        { handleNavigationCompleted(args); });
}

void WebView::Native::handleNavigationCompleted(
    ICoreWebView2NavigationCompletedEventArgs* args)
{
    loading = false;
    BOOL success = FALSE;
    args->get_IsSuccess(&success);

    if (success)
    {
        ComAllocatedWideString uri;
        webView->get_Source(&uri);
        owner.onNavigationFinished(uri.toString());
        return;
    }

    COREWEBVIEW2_WEB_ERROR_STATUS status;
    args->get_WebErrorStatus(&status);
    ComAllocatedWideString uri;
    webView->get_Source(&uri);
    auto errorStr = "Navigation failed (status=" + std::to_string(status)
                    + ") for url=" + uri.toString();
    LOG("WebView2: " + errorStr);
    owner.onNavigationFailed(errorStr);
}

void WebView::Native::addDocumentTitleChangedHandler()
{
    webView->add_DocumentTitleChanged(
        Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
            [this](ICoreWebView2*, IUnknown*) -> HRESULT
            {
                ComAllocatedWideString title;
                webView->get_DocumentTitle(&title);
                auto titleStr = title.toString();
                Threads::callAsync([cb = owner.onTitleChanged, titleStr]()
                                   { cb(titleStr); });
                return S_OK;
            })
            .Get(),
        &titleChangedToken);
}

void WebView::Native::addWebMessageReceivedHandler()
{
    forwardEventToMember<ICoreWebView2WebMessageReceivedEventHandler>(
        webView.Get(),
        &ICoreWebView2::add_WebMessageReceived,
        webMessageToken,
        [this](ICoreWebView2WebMessageReceivedEventArgs* args)
        { handleWebMessageReceived(args); });
}

void WebView::Native::handleWebMessageReceived(
    ICoreWebView2WebMessageReceivedEventArgs* args)
{
    if (!isFromTrustedSender(args))
        return;

    ComAllocatedWideString messageRaw;
    args->get_WebMessageAsJson(&messageRaw);
    if (!messageRaw)
        return;

    auto message = detail::parseWebMessageEnvelope(messageRaw.toString());
    if (!message)
        return;

    auto it = messageHandlers.find(message->name);
    if (it == messageHandlers.end())
        return;

    // Invoke directly on the UI thread (where this
    // callback already runs). Going through callAsync
    // would post onto the dispatcher queue, which on
    // Windows doesn't get drained while a nested
    // runEventLoopFor is pumping — so bridge messages
    // would never reach the handler and tests that
    // depend on bridge round-trips would silently see
    // no data.
    it->second(message->body);
}

void WebView::Native::addPermissionRequestedHandler()
{
    webView->add_PermissionRequested(
        Microsoft::WRL::Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*,
               ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT
            {
                auto kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
                args->get_PermissionKind(&kind);

                if (isMediaCapturePermission(kind))
                    args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);

                return S_OK;
            })
            .Get(),
        &permissionRequestedToken);
}

// window.open / target="_blank" -> hand the embedder a new app-owned
// WebView, mirroring the macOS createWebViewWithConfiguration delegate.
void WebView::Native::addNewWindowRequestedHandler()
{
    webView->add_NewWindowRequested(
        Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
            { return handleNewWindowRequested(args); })
            .Get(),
        &newWindowRequestedToken);
}

// JS window.close() -> notify the embedder (e.g. so it tears the popup
// window down). Matches the macOS webViewDidClose delegate.
void WebView::Native::addWindowCloseRequestedHandler()
{
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
WebView::Native::handleNewWindowRequested(
    ICoreWebView2NewWindowRequestedEventArgs* args)
{
    ComAllocatedWideString uri;
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
        args->put_Handled(TRUE);
        deferral->Complete();

        if (options.loadDeclinedPopupsInline)
            owner.loadURL(url);
    }

    return S_OK;
}
} // namespace eacp::Graphics
