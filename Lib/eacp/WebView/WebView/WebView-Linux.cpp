#include "WebView.h"

#include "GLibPump-Linux.h"
#include "MessageOrigin.h"
#include "WebViewDetail.h"
#include "WebViewSurface-Linux.h"
#include "WpeFrame-Linux.h"
#include "WpePlatform-Linux.h"

#include <eacp/Graphics/View/View-Linux.h>
#include <eacp/Graphics/Window/LinuxWindowSurface-Linux.h>

#include <wpe/webkit.h>

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eacp::Graphics
{
namespace
{
struct GObjectUnref
{
    void operator()(gpointer object) const
    {
        if (object != nullptr)
            g_object_unref(object);
    }
};

template <typename T>
using GObjectPtr = std::unique_ptr<T, GObjectUnref>;

struct GFreeString
{
    void operator()(char* text) const { g_free(text); }
};

using GOwnedString = std::unique_ptr<char, GFreeString>;

std::string safeString(const char* text)
{
    return text != nullptr ? std::string {text} : std::string {};
}

std::string takeString(char* text)
{
    return safeString(GOwnedString {text}.get());
}

// What the Apple backend makes of a script's result: a string as itself, a
// number as its decimal form, nothing for undefined and null, and anything
// else as JSON.
std::string scriptResultString(JSCValue* value)
{
    if (value == nullptr || jsc_value_is_undefined(value)
        || jsc_value_is_null(value))
        return {};

    if (jsc_value_is_string(value) || jsc_value_is_number(value)
        || jsc_value_is_boolean(value))
        return takeString(jsc_value_to_string(value));

    return takeString(jsc_value_to_json(value, 0));
}

// A posted string arrives as itself and an object or array as JSON; a bare
// scalar arrives empty, as on the other backends.
std::string messageBody(JSCValue* value)
{
    if (value == nullptr)
        return {};

    if (jsc_value_is_string(value))
        return takeString(jsc_value_to_string(value));

    if (jsc_value_is_object(value) || jsc_value_is_array(value))
        return takeString(jsc_value_to_json(value, 0));

    return {};
}

void scriptEvaluated(GObject* source, GAsyncResult* result, gpointer data)
{
    auto deliver = std::unique_ptr<WebView::JSCallback> {
        static_cast<WebView::JSCallback*>(data)};
    GError* error = nullptr;
    auto value = GObjectPtr<JSCValue> {webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(source), result, &error)};

    if (error == nullptr)
    {
        (*deliver)(scriptResultString(value.get()), {});
        return;
    }

    auto failure = safeString(error->message);
    g_error_free(error);

    (*deliver)({}, failure.empty() ? "Unknown error" : failure);
}

constexpr auto freshFrameWait = Time::MS {100};
} // namespace

struct WebView::Native
{
    struct MessageRoute
    {
        Native* native = nullptr;
        std::string name;
    };

    Native(WebView& ownerToUse, const Options& options)
        : owner(ownerToUse)
        , originPolicy(options)
    {
        detail::ensureGLibPump();

        auto* display = detail::wpeDisplay();

        if (display == nullptr)
            return;

        contentManager.reset(webkit_user_content_manager_new());

        if (options.ephemeralSession)
            session.reset(webkit_network_session_new_ephemeral());
        else
            session.reset(WEBKIT_NETWORK_SESSION(
                g_object_ref(webkit_network_session_get_default())));

        webView.reset(WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                                   "display",
                                                   display,
                                                   "user-content-manager",
                                                   contentManager.get(),
                                                   "network-session",
                                                   session.get(),
                                                   nullptr)));

        applySettings(options);
        connectSignals();

        surfaceView = std::make_unique<detail::WebViewSurface>(lastFrame);
        surfaceView->setTransparent(options.transparentBackground);
        surfaceView->onPresented = [this] { host->bufferPresented(); };
        surfaceView->onBackingScaleChanged = [this](float) { pushGeometry(); };
        owner.addSubview(*surfaceView);

        host = std::make_unique<detail::WpeViewHost>(
            webkit_web_view_get_wpe_view(webView.get()));
        host->setFrameSink([this](const detail::WpeFrameBytes& frame)
                           { frameRendered(frame); });
    }

    ~Native()
    {
        if (surfaceView != nullptr)
        {
            surfaceView->onPresented = [] {};
            owner.removeSubview(*surfaceView);
            surfaceView.reset();
        }

        host.reset();

        if (contentManager != nullptr)
        {
            g_signal_handlers_disconnect_matched(contentManager.get(),
                                                 G_SIGNAL_MATCH_FUNC,
                                                 0,
                                                 0,
                                                 nullptr,
                                                 (gpointer) scriptMessageReceived,
                                                 nullptr);

            for (auto& [name, _]: messageSignals)
                webkit_user_content_manager_unregister_script_message_handler(
                    contentManager.get(), name.c_str(), nullptr);
        }

        if (webView != nullptr)
            g_signal_handlers_disconnect_by_data(webView.get(), this);
    }

    void applySettings(const Options& options)
    {
        auto* settings = webkit_web_view_get_settings(webView.get());
        webkit_settings_set_enable_developer_extras(settings, options.debugConsole);

        if (!options.userAgent.empty())
            webkit_settings_set_user_agent(settings, options.userAgent.c_str());

        if (options.transparentBackground)
        {
            auto clear = WebKitColor {0.0, 0.0, 0.0, 0.0};
            webkit_web_view_set_background_color(webView.get(), &clear);
        }
    }

    static void onLoadChanged(WebKitWebView*, WebKitLoadEvent event, gpointer data)
    {
        static_cast<Native*>(data)->loadChanged(event);
    }

    static gboolean onLoadFailed(
        WebKitWebView*, WebKitLoadEvent, const char*, GError* error, gpointer data)
    {
        static_cast<Native*>(data)->loadFailed(error);
        return FALSE;
    }

    static void onTitleNotify(GObject*, GParamSpec*, gpointer data)
    {
        static_cast<Native*>(data)->titleChanged();
    }

    void connectSignals()
    {
        auto* view = webView.get();
        g_signal_connect(view, "load-changed", G_CALLBACK(onLoadChanged), this);
        g_signal_connect(view, "load-failed", G_CALLBACK(onLoadFailed), this);
        g_signal_connect(view, "notify::title", G_CALLBACK(onTitleNotify), this);
    }

    template <typename Callback>
    void deliverLater(Callback callback)
    {
        Threads::callAsync(
            [weak = self, run = std::move(callback)]
            {
                if (auto native = weak.lock())
                    run(*native);
            });
    }

    void loadChanged(WebKitLoadEvent event)
    {
        auto url = currentURL();

        if (event == WEBKIT_LOAD_STARTED)
        {
            navigationFailed = false;
            deliverLater([url](Native& native)
                         { native.owner.onNavigationStarted(url); });
        }
        else if (event == WEBKIT_LOAD_FINISHED && !navigationFailed)
        {
            deliverLater([url](Native& native)
                         { native.owner.onNavigationFinished(url); });
        }
    }

    void loadFailed(GError* error)
    {
        navigationFailed = true;
        auto message = error != nullptr ? safeString(error->message)
                                        : std::string {"Unknown error"};

        deliverLater([message](Native& native)
                     { native.owner.onNavigationFailed(message); });
    }

    void titleChanged()
    {
        auto title = safeString(webkit_web_view_get_title(webView.get()));
        deliverLater([title](Native& native)
                     { native.owner.onTitleChanged(title); });
    }

    std::string currentURL() const
    {
        if (webView == nullptr)
            return {};

        return safeString(webkit_web_view_get_uri(webView.get()));
    }

    static void scriptMessageReceived(WebKitUserContentManager*,
                                      JSCValue* value,
                                      gpointer data)
    {
        auto* route = static_cast<MessageRoute*>(data);
        route->native->messageReceived(route->name, value);
    }

    // WebKit hands the handler no frame, so the sender is the view's own
    // document: its URI is the origin, and about:blank is what loadHTML
    // without a base URL leaves there.
    void messageReceived(const std::string& name, JSCValue* value)
    {
        auto url = currentURL();

        if (!originPolicy.admit(detail::originOf(url), url == "about:blank"))
            return;

        deliverLater(
            [name, body = messageBody(value)](Native& native)
            {
                auto found = native.messageHandlers.find(name);

                if (found != native.messageHandlers.end())
                    found->second(body);
            });
    }

    void routeMessages(const std::string& name)
    {
        if (contentManager == nullptr || messageSignals.contains(name))
            return;

        webkit_user_content_manager_register_script_message_handler(
            contentManager.get(), name.c_str(), nullptr);

        auto signal = "script-message-received::" + name;
        auto id = g_signal_connect_data(
            contentManager.get(),
            signal.c_str(),
            G_CALLBACK(scriptMessageReceived),
            new MessageRoute {this, name},
            [](gpointer data, GClosure*)
            { delete static_cast<MessageRoute*>(data); },
            GConnectFlags {});

        messageSignals[name] = id;
    }

    void stopRoutingMessages(const std::string& name)
    {
        auto found = messageSignals.find(name);

        if (found == messageSignals.end())
            return;

        g_signal_handler_disconnect(contentManager.get(), found->second);
        webkit_user_content_manager_unregister_script_message_handler(
            contentManager.get(), name.c_str(), nullptr);
        messageSignals.erase(found);
    }

    // On screen, WebKit's next frame waits for this one's present; anywhere
    // else (headless, hidden, not yet mapped) the frame clock paces it.
    void frameRendered(const detail::WpeFrameBytes& frame)
    {
        lastFrame.store(frame.bgra, frame.width, frame.height, frame.stride);

        auto presenting = surfaceView->canPresent();
        host->setPresenting(presenting);

        if (presenting)
            surfaceView->frameArrived();

        deliverSnapshots();
    }

    // A snapshot waits briefly for a frame rendered after it was asked for,
    // so a page that has just changed is caught after the change; a page that
    // is not repainting answers with the frame it last rendered.
    void requestSnapshot(SnapshotCallback callback)
    {
        waitingSnapshots.push_back(std::move(callback));

        Threads::callAfter(freshFrameWait,
                           [weak = self]
                           {
                               if (auto native = weak.lock())
                                   native->deliverSnapshots();
                           });
    }

    void deliverSnapshots()
    {
        if (lastFrame.isEmpty() || waitingSnapshots.empty())
            return;

        auto png = Bytes {};
        auto error = std::string {};

        try
        {
            png = lastFrame.toImage().toPng();
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }

        for (auto& callback: std::exchange(waitingSnapshots, {}))
            Threads::callAsync([callback, png, error] { callback(png, error); });
    }

    void pushGeometry()
    {
        if (host == nullptr)
            return;

        auto* window = linuxWindowForView(owner);
        auto scale = window != nullptr ? window->scale : linuxDefaultBackingScale;
        auto bounds = owner.getLocalBounds();

        host->resize(bounds.w, bounds.h, scale);
    }

    WebView& owner;
    std::weak_ptr<Native> self;
    detail::MessageOriginPolicy originPolicy;
    GObjectPtr<WebKitUserContentManager> contentManager;
    GObjectPtr<WebKitNetworkSession> session;
    GObjectPtr<WebKitWebView> webView;
    std::unique_ptr<detail::WpeViewHost> host;
    detail::WpeFrame lastFrame;
    std::unique_ptr<detail::WebViewSurface> surfaceView;
    std::vector<SnapshotCallback> waitingSnapshots;
    std::unordered_map<std::string, MessageFunc> messageHandlers;
    std::unordered_map<std::string, gulong> messageSignals;
    bool navigationFailed = false;
    bool contentFocused = false;
};

void WebView::initNative(Options options)
{
    impl = std::make_shared<Native>(*this, options);
    impl->self = impl;
    detail::registerWebView(this);

    installWindowDragSupport();
    installWindowControlSupport();

    if (options.driveOffscreenAnimation)
        installOffscreenAnimationSupport();
}

WebView::~WebView()
{
    detail::unregisterWebView(this);
}

void WebView::loadURL(const std::string& url)
{
    if (impl->webView != nullptr)
        webkit_web_view_load_uri(impl->webView.get(), url.c_str());
}

void WebView::loadHTML(const std::string& html, const std::string& baseURL)
{
    if (baseURL.empty())
        impl->originPolicy.trustInlineDocument();

    if (impl->webView != nullptr)
        webkit_web_view_load_html(impl->webView.get(),
                                  html.c_str(),
                                  baseURL.empty() ? nullptr : baseURL.c_str());
}

void WebView::goBack()
{
    if (impl->webView != nullptr)
        webkit_web_view_go_back(impl->webView.get());
}

void WebView::goForward()
{
    if (impl->webView != nullptr)
        webkit_web_view_go_forward(impl->webView.get());
}

void WebView::reload()
{
    if (impl->webView != nullptr)
        webkit_web_view_reload(impl->webView.get());
}

void WebView::stopLoading()
{
    if (impl->webView != nullptr)
        webkit_web_view_stop_loading(impl->webView.get());
}

bool WebView::canGoBack() const
{
    return impl->webView != nullptr
           && webkit_web_view_can_go_back(impl->webView.get());
}

bool WebView::canGoForward() const
{
    return impl->webView != nullptr
           && webkit_web_view_can_go_forward(impl->webView.get());
}

bool WebView::isLoading() const
{
    return impl->webView != nullptr
           && webkit_web_view_is_loading(impl->webView.get());
}

std::string WebView::getURL() const
{
    return impl->currentURL();
}

std::string WebView::getTitle() const
{
    if (impl->webView == nullptr)
        return {};

    return safeString(webkit_web_view_get_title(impl->webView.get()));
}

void WebView::evaluateJavaScript(const std::string& script,
                                 const JSCallback& callback)
{
    Threads::assertMainThread();

    if (impl->webView == nullptr)
    {
        if (callback)
            Threads::callAsync([callback]
                               { callback({}, "WebView not initialized"); });
        return;
    }

    webkit_web_view_evaluate_javascript(impl->webView.get(),
                                        script.c_str(),
                                        (gssize) script.size(),
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        callback ? scriptEvaluated : nullptr,
                                        callback ? new JSCallback {callback}
                                                 : nullptr);
}

void WebView::takeSnapshot(SnapshotCallback callback)
{
    if (!callback)
        return;

    impl->requestSnapshot(detail::withSnapshotDeadline(std::move(callback)));
}

void WebView::setZoom(double level)
{
    if (impl->webView != nullptr)
        webkit_web_view_set_zoom_level(impl->webView.get(),
                                       detail::clampZoom(level));
}

double WebView::getZoom() const
{
    if (impl->webView == nullptr)
        return 1.0;

    return webkit_web_view_get_zoom_level(impl->webView.get());
}

void WebView::focusContent()
{
    focus();

    for (auto* view: detail::registeredWebViews())
        if (view != this && view->impl->contentFocused)
        {
            view->impl->contentFocused = false;

            if (view->impl->host != nullptr)
                view->impl->host->setFocused(false);
        }

    impl->contentFocused = true;

    if (impl->host != nullptr)
        impl->host->setFocused(true);
}

WebView* WebView::focused()
{
    for (auto* view: detail::registeredWebViews())
        if (view->impl != nullptr && view->impl->contentFocused)
            return view;

    return nullptr;
}

bool WebView::isRuntimeAvailable()
{
    return true;
}

void WebView::addScriptMessageHandler(const std::string& name,
                                      const MessageFunc& handler)
{
    impl->messageHandlers[name] = handler;
    impl->routeMessages(name);
}

void WebView::removeScriptMessageHandler(const std::string& name)
{
    impl->messageHandlers.erase(name);
    impl->stopRoutingMessages(name);
}

void WebView::addUserScript(const std::string& source, bool atDocumentStart)
{
    if (impl->contentManager == nullptr)
        return;

    auto injectionTime = atDocumentStart
                             ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
                             : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;

    auto* script = webkit_user_script_new(source.c_str(),
                                          WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                          injectionTime,
                                          nullptr,
                                          nullptr);

    webkit_user_content_manager_add_script(impl->contentManager.get(), script);
    webkit_user_script_unref(script);
}

void WebView::armFileDrag(const Vector<std::string>&) {}

void WebView::armWindowDrag() {}

void WebView::performWindowControl(const std::string&) {}

void WebView::installKeyEventSupport() {}

void WebView::resized()
{
    View::resized();

    if (impl->surfaceView != nullptr)
        impl->surfaceView->setBounds(getLocalBounds());

    impl->pushGeometry();
}

void* WebView::nativeFocusTarget()
{
    return View::nativeFocusTarget();
}

void WebView::hostWindowMoved() {}

void WebView::hostWindowVisibilityChanged(bool) {}

void WebView::visibilityChanged(bool visible)
{
    if (impl->host != nullptr)
        impl->host->setVisible(visible);
}

void WebView::mouseDown(const MouseEvent&) {}
void WebView::mouseUp(const MouseEvent&) {}
void WebView::mouseDragged(const MouseEvent&) {}
void WebView::mouseMoved(const MouseEvent&) {}
void WebView::mouseExited(const MouseEvent&) {}
void WebView::mouseWheel(const MouseEvent&) {}

bool WebView::fileDragEnter(const Vector<std::string>&, Point)
{
    return false;
}

bool WebView::fileDragMove(const Vector<std::string>&, Point)
{
    return false;
}

void WebView::fileDragExit() {}
void WebView::filesDropped(const Vector<std::string>&, Point) {}
} // namespace eacp::Graphics
