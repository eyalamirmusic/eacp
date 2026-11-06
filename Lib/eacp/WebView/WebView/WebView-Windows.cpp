#include "WebView-Windows.h"

#include "WebViewMessages.h"
#include <eacp/Graphics/DComp-Windows.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Threads/ThreadUtils.h>

#include <objbase.h>

namespace eacp::Graphics
{

namespace
{
void deliverScriptResult(const WebView::JSCallback& callback,
                         HRESULT errorCode,
                         LPCWSTR resultJson)
{
    if (FAILED(errorCode))
    {
        callback("", "Script execution failed");
        return;
    }

    auto result = std::string {};

    if (resultJson)
        result = detail::decodeScriptResult(Strings::narrow(resultJson));

    callback(result, "");
}

// Reads a stream the capture has just written back from its start.
std::optional<Bytes> readStreamFromStart(IStream* stream)
{
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    STATSTG stat = {};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)))
        return std::nullopt;

    auto bytes = Bytes {};
    bytes.resize((int) stat.cbSize.LowPart);
    ULONG read = 0;
    stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
    bytes.resize((int) read);
    return bytes;
}
} // namespace

WebView::Native::Native(WebView& ownerToUse, WebView::Options optionsToUse)
    : owner(ownerToUse)
    , options(std::move(optionsToUse))
{
}

WebView::Native::~Native()
{
    // Fences the queued create retry (see retryWebView2Create) against a
    // Native that went away before the event loop got to it.
    *alive = false;

    // Unhook our event handlers before tearing the controller down
    // so any late callback from the browser process can't fire
    // back into a half-destroyed Native.
    removeEventHandlers();

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

    detachRenderVisual();
}

// Removes the render visual from the View's composition tree.
void WebView::Native::detachRenderVisual()
{
    if (!webViewVisual)
        return;

    if (auto* container = static_cast<IDCompositionVisual2*>(owner.getNativeLayer()))
        container->RemoveVisual(webViewVisual.Get());

    webViewVisual.Reset();
    commitComposition();
}

void WebView::Native::updateBounds()
{
    if (!webViewVisual)
        return;

    auto bounds = owner.getLocalBounds();
    auto dpiScale = getDpiScale();

    auto widthPx = bounds.w * dpiScale;
    auto heightPx = bounds.h * dpiScale;

    // The render visual lives inside the WebView's own View container, which
    // the View tree already positions and (via the Panel) applies opacity
    // to. So it only needs to sit at the container origin.
    //
    // WebView2 treats the RootVisualTarget's coordinate space as physical
    // pixels, but our composition root is already DPI-scaled. Counter-scale
    // the visual by 1/dpi so the browser's pixel-space content maps back to
    // the correct logical size on screen (without this it renders dpi-times
    // too large on high-DPI displays). DComp visuals carry no size — the
    // extent comes from the content WebView2 renders, and from the bounds
    // handed to put_Bounds below.
    webViewVisual->SetOffsetX(0.0f);
    webViewVisual->SetOffsetY(0.0f);

    if (dpiScale > 0.f)
        webViewVisual->SetTransform(
            D2D1::Matrix3x2F::Scale(1.0f / dpiScale, 1.0f / dpiScale));

    commitComposition();

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

float WebView::Native::getDpiScale()
{
    if (hostHwnd)
        return static_cast<float>(GetDpiForWindow(hostHwnd)) / 96.f;
    return 1.f;
}

// Visual hosting still leaves WebView2 a top-level window of its own for
// input, IME and accessibility, which it places in screen coordinates
// derived from the parent HWND. It re-derives that placement only when
// told: without this the window keeps the position the host had when the
// controller was created, so it is left behind wherever the window opened.
void WebView::Native::notifyParentWindowMoved()
{
    if (controller)
        controller->NotifyParentWindowPositionChanged();
}

// That same window is shown and hidden independently of the host, so a
// hidden window (a tray app's panel, a minimized window) otherwise leaves
// it behind on screen — still hit-testable, and still compositing a page
// nobody can see. The host's visibility is remembered rather than read
// back, because it can change while the controller is still initializing —
// and because a window that has never been shown is not thereby hidden:
// an offscreen snapshot of one still needs the page rendering.
void WebView::Native::setControllerVisible(bool visibleToUse)
{
    hostVisible = visibleToUse;
    applyControllerVisibility();
}

// The other half of the same question: View::setVisible on this view or an
// ancestor. Tracked apart from the host window's state because either one
// alone is enough to hide the page, and neither may undo the other — a
// parked service view must stay hidden when its window is shown again.
void WebView::Native::setViewVisible(bool visibleToUse)
{
    viewVisible = visibleToUse;
    applyControllerVisibility();
}

void WebView::Native::applyControllerVisibility()
{
    if (controller)
        controller->put_IsVisible(hostVisible && viewVisible ? TRUE : FALSE);
}

void WebView::Native::queueOperation(std::function<void()> op)
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
void WebView::Native::queueDocStartScript(std::wstring script)
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

void WebView::Native::processPendingOperations()
{
    for (auto& script: pendingDocStartScripts)
    {
        if (webView)
            webView->AddScriptToExecuteOnDocumentCreated(script.c_str(), nullptr);
    }
    pendingDocStartScripts.clear();

    while (!pendingOperations.empty())
    {
        auto op = std::move(pendingOperations.front());
        pendingOperations.pop();
        op();
    }
}

void WebView::Native::evaluateScript(const std::string& script,
                                     const WebView::JSCallback& callback)
{
    if (!webView)
    {
        if (callback)
            callback("", "WebView not initialized");
        return;
    }

    auto wideScript = Strings::widen(script);

    webView->ExecuteScript(
        wideScript.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [callback](HRESULT errorCode, LPCWSTR resultJson) -> HRESULT
            {
                if (callback)
                    deliverScriptResult(callback, errorCode, resultJson);
                return S_OK;
            })
            .Get());
}

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
                auto wideUrl = Strings::widen(url);
                impl->webView->Navigate(wideUrl.c_str());
            }
        });
}

void WebView::loadHTML(const std::string& html, const std::string& baseURL)
{
    if (baseURL.empty())
        impl->originPolicy.trustInlineDocument();

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

            auto wideHtml = Strings::widen(html);
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

    // Before ensureInitialized, so the deadline also covers an initialization
    // that never finishes and leaves the queued operation unrun.
    callback = detail::withSnapshotDeadline(std::move(callback));

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
                        auto bytes = Bytes {};
                        auto error = std::string {};

                        if (FAILED(errorCode))
                            error = "CapturePreview failed";
                        else if (auto captured = readStreamFromStart(stream.Get()))
                            bytes = std::move(*captured);
                        else
                            error = "IStream::Stat failed";

                        Threads::callAsync(
                            [callback, pngBytes = std::move(bytes), error]() mutable
                            { callback(std::move(pngBytes), error); });
                        return S_OK;
                    })
                    .Get());
        });
}

void WebView::addScriptMessageHandler(const std::string& name,
                                      const MessageFunc& handler)
{
    impl->messageHandlers[name] = handler;

    // Expose the handler under both the plain `window.<name>` form and the
    // WebKit `window.webkit.messageHandlers.<name>` form. macOS only offers the
    // latter, so mirroring it here lets the same page code post messages on
    // both platforms without branching.
    auto script = Strings::widen(detail::buildMessageHandlerScript(name));

    impl->ensureInitialized();

    // A document-start script only reaches documents created after it, but
    // WKWebView exposes a handler to the page already loaded too. Mirror that:
    // a handler added after the first navigation (e.g. a channel attached on
    // demand) must work without a reload. The script is idempotent, so
    // running it on the live page as well as on later loads is harmless.
    if (impl->initialized && impl->webView)
        impl->webView->ExecuteScript(script.c_str(), nullptr);

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
        impl->queueDocStartScript(Strings::widen(source));
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

void WebView::hostWindowVisibilityChanged(bool visibleToUse)
{
    impl->setControllerVisible(visibleToUse);
}

void WebView::visibilityChanged(bool visibleToUse)
{
    impl->setViewVisible(visibleToUse);
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
