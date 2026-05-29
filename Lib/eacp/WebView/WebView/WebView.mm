#import <WebKit/WebKit.h>
#import <Foundation/Foundation.h>
#include "Snapshot-Apple.h"
#include "WebView.h"
#include "WebViewPlatform-Apple.h"

#include <eacp/Core/ObjC/Strings.h>
#include <eacp/Graphics/Primitives/GraphicUtils.h>
#include <ea_data_structures/Structures/Vector.h>
#include <algorithm>
#include <unordered_map>

namespace
{
std::string safeString(const char* str, const char* fallback = "")
{
    return str != nullptr ? str : fallback;
}
} // namespace

#if EACP_WEBVIEW_PRIVATE_MEDIA_CAPTURE_SPI
// Private WebKit SPI forward declaration. WKUIDelegate didn't expose a public
// camera/microphone permission selector until macOS 12 — on macOS 11 WebKit
// only calls this underscored variant. Mirrors the enum WebKit ships in its
// _WKWebsiteDataStore / WKUIDelegatePrivate headers; bit values are part of
// the ABI and must not change.
typedef NS_OPTIONS(NSUInteger, _WKCaptureDevices) {
    _WKCaptureDeviceMicrophone = 1 << 0,
    _WKCaptureDeviceCamera = 1 << 1,
    _WKCaptureDeviceDisplay = 1 << 2,
};
#endif

namespace eacp::Graphics
{
class WebView;
}

using MessageHandlerMap =
    std::unordered_map<std::string, std::function<void(const std::string&)>>;

@interface WebViewDelegate
    : NSObject <WKNavigationDelegate, WKScriptMessageHandler, WKUIDelegate>
{
@public
    std::weak_ptr<eacp::Graphics::WebView::Native> nativeWeak;
}
@property(assign) MessageHandlerMap* messageHandlers;
@end

@interface ResourceSchemeHandler : NSObject <WKURLSchemeHandler>
{
@public
    eacp::Graphics::ResourceProvider provider;
}
@end

namespace
{
// Script-message channel + injected JS for native file drag-out. Kept private
// to the macOS TU; the page reaches it via `window.eacpArmFileDrag(...)`.
//
// The helper lives on its own global -- NOT under `window.eacp`, which is the
// framework RPC bridge's namespace. The bridge bails out of its init if
// `window.eacp` already exists (Bridge.cpp), so a document-start script that
// pre-creates `window.eacp` would silently kill the bridge.
//
// Contract: the page calls eacpArmFileDrag from a `mousedown` handler to arm
// the drag for the current gesture. The native side starts the actual drag
// from the real mouseDragged: event once the pointer crosses the drag
// threshold. The element must NOT be draggable="true" and must NOT use
// `dragstart` -- a competing WebKit HTML5 drag would conflict with the native
// one.
NSString* const fileDragMessageName = @"__eacpFileDrag";

NSString* const fileDragUserScript =
    @"window.eacpArmFileDrag = function (payload) {"
    @"  try {"
    @"    window.webkit.messageHandlers.__eacpFileDrag.postMessage("
    @"      payload == null ? '' : String(payload));"
    @"  } catch (e) {}"
    @"};";
} // namespace

namespace eacp::Graphics
{
struct WebView::PopupInit
{
    WKWebViewConfiguration* configuration = nil;
    bool inspectable = false;
};

struct WebView::Native
{
    Native(WebView& ownerToUse, Options options)
        : owner(ownerToUse)
    {
        delegate = [[WebViewDelegate alloc] init];
        delegate.get().messageHandlers = &messageHandlers;

        config = [[WKWebViewConfiguration alloc] init];

        if (options.debugConsole)
        {
            [config.get().preferences setValue:@YES
                                        forKey:@"developerExtrasEnabled"];
        }

        for (auto& [scheme, provider]: options.schemes)
        {
            auto handler = ObjC::Ptr {[[ResourceSchemeHandler alloc] init]};
            handler.get()->provider = std::move(provider);
            [config.get() setURLSchemeHandler:handler.get()
                                 forURLScheme:Strings::toNSString(scheme)];
            schemeHandlers.push_back(std::move(handler));
        }

        webView = detail::createWebView(config.get());
        webView.get().navigationDelegate = delegate.get();
        webView.get().UIDelegate = delegate.get();

        [webView.get() addObserver:delegate.get()
                        forKeyPath:@"title"
                           options:NSKeyValueObservingOptionNew
                           context:nullptr];
        observingTitle = true;

        if (options.debugConsole)
        {
            if (@available(macOS 13.3, iOS 16.4, *))
                webView.get().inspectable = YES;
        }

        installFileDragBridge();
    }

    Native(WebView& ownerToUse, WebView::PopupInit init)
        : owner(ownerToUse)
    {
        delegate = [[WebViewDelegate alloc] init];
        delegate.get().messageHandlers = &messageHandlers;

        config = ObjC::Ptr {init.configuration, ObjC::RetainMode {}};

        auto rect = CGRectMake(0, 0, 100, 100);
        webView = [[WKWebView alloc] initWithFrame:rect configuration:config.get()];
        webView.get().navigationDelegate = delegate.get();
        webView.get().UIDelegate = delegate.get();

        [webView.get() addObserver:delegate.get()
                        forKeyPath:@"title"
                           options:NSKeyValueObservingOptionNew
                           context:nullptr];
        observingTitle = true;

        if (init.inspectable)
        {
            if (@available(macOS 13.3, iOS 16.4, *))
                webView.get().inspectable = YES;
        }
    }
    ~Native()
    {
        auto controller = config.get().userContentController;

        for (auto& [name, _]: messageHandlers)
        {
            [controller removeScriptMessageHandlerForName:Strings::toNSString(name)];
        }

        [controller removeScriptMessageHandlerForName:fileDragMessageName];

        if (observingTitle)
            [webView.get() removeObserver:delegate.get() forKeyPath:@"title"];

        webView.get().navigationDelegate = nil;
        webView.get().UIDelegate = nil;
    }

    void attachToParentView()
    {
        auto* parentHandle = owner.getHandle();

        if (parentHandle != nullptr)
            detail::attachWKWebViewToParent(webView.get(), parentHandle);
    }
    void updateFrame()
    {
        auto bounds = owner.getLocalBounds();
        webView.get().frame = toCGRect(bounds);
    }

    void installFileDragBridge()
    {
        auto* controller = config.get().userContentController;
        [controller addScriptMessageHandler:delegate.get()
                                       name:fileDragMessageName];

        auto* script = [[WKUserScript alloc]
              initWithSource:fileDragUserScript
               injectionTime:WKUserScriptInjectionTimeAtDocumentStart
            forMainFrameOnly:NO];
        [controller addUserScript:script];
    }

    // Resolve the drag payload to file paths now (cheap -- just strings; the
    // real copy is deferred to the file promise) and arm the next gesture. The
    // platform view starts the session from the real mouseDragged: event.
    void armFileDrag(const std::string& payload)
    {
        auto paths = owner.onFileDragRequested(payload);
        detail::armFileDrag(webView.get(), paths);
    }

    ObjC::Ptr<WKWebView> webView;
    ObjC::Ptr<WebViewDelegate> delegate;
    ObjC::Ptr<WKWebViewConfiguration> config;
    EA::Vector<ObjC::Ptr<ResourceSchemeHandler>> schemeHandlers;
    MessageHandlerMap messageHandlers;
    WebView& owner;
    double zoomLevel = 1.0;
    bool observingTitle = false;
};

struct WebViewNativeAccess
{
    static EA::OwningPointer<WebView>
        makePopup(WKWebViewConfiguration* configuration, bool inspectable)
    {
        auto init = WebView::PopupInit {configuration, inspectable};
        return EA::OwningPointer<WebView> {new WebView {init}};
    }

    static WKWebView* wkWebViewOf(WebView& popup)
    {
        return popup.impl != nullptr ? popup.impl->webView.get() : nil;
    }
};

} // namespace eacp::Graphics

@implementation WebViewDelegate

- (void)webView:(WKWebView*)webView
    didStartProvisionalNavigation:(WKNavigation*)navigation
{
    auto url = safeString([webView.URL.absoluteString UTF8String]);
    eacp::Threads::callAsync([weak = nativeWeak, url]() {
        if (auto native = weak.lock())
            native->owner.onNavigationStarted(url);
    });
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation
{
    auto url = safeString([webView.URL.absoluteString UTF8String]);
    eacp::Threads::callAsync([weak = nativeWeak, url]() {
        if (auto native = weak.lock())
            native->owner.onNavigationFinished(url);
    });
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error
{
    auto errorStr =
        safeString([error.localizedDescription UTF8String], "Unknown error");
    eacp::Threads::callAsync([weak = nativeWeak, errorStr]() {
        if (auto native = weak.lock())
            native->owner.onNavigationFailed(errorStr);
    });
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
            withError:(NSError*)error
{
    auto errorStr =
        safeString([error.localizedDescription UTF8String], "Unknown error");
    eacp::Threads::callAsync([weak = nativeWeak, errorStr]() {
        if (auto native = weak.lock())
            native->owner.onNavigationFailed(errorStr);
    });
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context
{
    if (! [keyPath isEqualToString:@"title"])
        return;

    auto* webView = (WKWebView*) object;
    auto title = safeString([webView.title UTF8String]);
    eacp::Threads::callAsync([weak = nativeWeak, title]() {
        if (auto native = weak.lock())
            native->owner.onTitleChanged(title);
    });
}

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)navigationAction
                    windowFeatures:(WKWindowFeatures*)windowFeatures
{
    auto url = safeString([navigationAction.request.URL.absoluteString UTF8String]);

    auto inspectable = NO;
    if (@available(macOS 13.3, iOS 16.4, *))
        inspectable = webView.inspectable;

    auto native = nativeWeak.lock();
    if (! native)
        return nil;

    auto popup = eacp::Graphics::WebViewNativeAccess::makePopup(configuration,
                                                                inspectable);
    auto* popupWKWebView = eacp::Graphics::WebViewNativeAccess::wkWebViewOf(*popup);

    if (native->owner.onNewWindowRequested(std::move(popup), url))
        return popupWKWebView;

    // Embedder declined. Load the URL inline so target="_blank"-style
    // navigations still reach the user.
    if (navigationAction.targetFrame == nil)
        [webView loadRequest:navigationAction.request];

    return nil;
}

- (void)webViewDidClose:(WKWebView*)webView
{
    eacp::Threads::callAsync([weak = nativeWeak]() {
        if (auto native = weak.lock())
            native->owner.onClose();
    });
}

- (void)webView:(WKWebView*)webView
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin*)origin
                          initiatedByFrame:(WKFrameInfo*)frame
                                      type:(WKMediaCaptureType)type
                           decisionHandler:
                               (void (^)(WKPermissionDecision decision))handler
    API_AVAILABLE(macos(12.0), ios(15.0))
{
    handler(WKPermissionDecisionGrant);
}

#if EACP_WEBVIEW_PRIVATE_MEDIA_CAPTURE_SPI
// macOS 11 fallback. WebKit on macOS 12+ prefers the public selector above;
// macOS 11 has no public selector and calls this one instead. The availability
// guard makes the preference explicit: if some future WebKit ever dispatched
// both, the public path still wins and we'd no-op here. handler() is always
// invoked so the request never hangs.
- (void)_webView:(WKWebView*)webView
    requestUserMediaAuthorizationForDevices:(_WKCaptureDevices)devices
                                        url:(NSURL*)url
                               mainFrameURL:(NSURL*)mainFrameURL
                            decisionHandler:(void (^)(BOOL authorized))handler
{
    if (@available(macOS 12.0, *))
    {
        handler(NO);
        return;
    }
    handler(YES);
}
#endif

- (void)userContentController:(WKUserContentController*)userContentController
      didReceiveScriptMessage:(WKScriptMessage*)message
{
    if ([message.name isEqualToString:fileDragMessageName])
    {
        // Arms the drag for the current gesture. The native WKWebView subclass
        // starts the actual session from the real mouseDragged: event, so this
        // message just needs to arrive before the pointer crosses the drag
        // threshold -- the press-to-move gap easily covers the round trip.
        auto native = nativeWeak.lock();
        if (native)
        {
            std::string payload;
            if ([message.body isKindOfClass:[NSString class]])
                payload = [message.body UTF8String];

            native->armFileDrag(payload);
        }
        return;
    }

    if (_messageHandlers)
    {
        auto name = std::string([message.name UTF8String]);
        auto it = _messageHandlers->find(name);
        if (it != _messageHandlers->end())
        {
            std::string body;
            if ([message.body isKindOfClass:[NSString class]])
            {
                body = [message.body UTF8String];
            }
            else
            {
                NSError* error = nil;
                NSData* data = [NSJSONSerialization dataWithJSONObject:message.body
                                                               options:0
                                                                 error:&error];
                if (data && !error)
                {
                    body = std::string((const char*) data.bytes, data.length);
                }
            }

            auto handler = it->second;
            eacp::Threads::callAsync([handler, body]() { handler(body); });
        }
    }
}

@end

@implementation ResourceSchemeHandler

- (void)webView:(WKWebView*)webView
    startURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask
{
    auto* url = urlSchemeTask.request.URL.absoluteString;
    auto urlStr = std::string([url UTF8String]);

    auto response = provider ? provider(urlStr) : std::nullopt;

    if (!response)
    {
        auto* error =
            [NSError errorWithDomain:NSURLErrorDomain
                                code:NSURLErrorResourceUnavailable
                            userInfo:nil];
        [urlSchemeTask didFailWithError:error];
        return;
    }

    auto* mime = eacp::Strings::toNSString(response->mimeType);
    auto* httpResponse =
        [[NSHTTPURLResponse alloc] initWithURL:urlSchemeTask.request.URL
                                    statusCode:response->statusCode
                                   HTTPVersion:@"HTTP/1.1"
                                  headerFields:@{@"Content-Type": mime}];

    [urlSchemeTask didReceiveResponse:httpResponse];

    auto* data = [NSData dataWithBytes:response->data.data()
                                length:response->data.size()];
    [urlSchemeTask didReceiveData:data];
    [urlSchemeTask didFinish];
}

- (void)webView:(WKWebView*)webView
    stopURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask
{
}

@end

namespace eacp::Graphics
{
namespace detail
{
EA::Vector<WebView*>& registeredWebViews()
{
    static EA::Vector<WebView*> instances;
    return instances;
}

WKWebView* wkWebViewOf(WebView* view)
{
    return view != nullptr ? WebViewNativeAccess::wkWebViewOf(*view) : nil;
}
} // namespace detail

namespace
{
void registerWebView(WebView* view)
{
    detail::registeredWebViews().add(view);
}

void unregisterWebView(WebView* view)
{
    detail::registeredWebViews().removeAllMatches(view);
}
} // namespace

void WebView::initNative(Options options)
{
    impl = std::make_shared<Native>(*this, std::move(options));
    impl->delegate.get()->nativeWeak = impl;
    impl->attachToParentView();
    registerWebView(this);
}

WebView::WebView(PopupInit init)
{
    impl = std::make_shared<Native>(*this, init);
    impl->delegate.get()->nativeWeak = impl;
    impl->attachToParentView();
    registerWebView(this);
}

WebView::~WebView()
{
    unregisterWebView(this);
    [impl->webView.get() removeFromSuperview];
}

void WebView::loadURL(const std::string& url)
{
    auto* nsUrl = [NSURL URLWithString:Strings::toNSString(url)];
    auto* request = [NSURLRequest requestWithURL:nsUrl];
    [impl->webView.get() loadRequest:request];
}

void WebView::loadHTML(const std::string& html, const std::string& baseURL)
{
    auto* nsHtml = [NSString stringWithUTF8String:html.c_str()];
    NSURL* nsBaseURL = nil;
    if (!baseURL.empty())
    {
        nsBaseURL =
            [NSURL URLWithString:[NSString stringWithUTF8String:baseURL.c_str()]];
    }
    [impl->webView.get() loadHTMLString:nsHtml baseURL:nsBaseURL];
}

void WebView::goBack()
{
    [impl->webView.get() goBack];
}

void WebView::goForward()
{
    [impl->webView.get() goForward];
}

void WebView::reload()
{
    [impl->webView.get() reload];
}

void WebView::stopLoading()
{
    [impl->webView.get() stopLoading];
}

bool WebView::canGoBack() const
{
    return impl->webView.get().canGoBack;
}

bool WebView::canGoForward() const
{
    return impl->webView.get().canGoForward;
}

bool WebView::isLoading() const
{
    return impl->webView.get().isLoading;
}

std::string WebView::getURL() const
{
    auto* url = impl->webView.get().URL;

    if (url == nil)
        return "";

    return safeString([url.absoluteString UTF8String]);
}

std::string WebView::getTitle() const
{
    auto* title = impl->webView.get().title;

    if (title == nil)
        return "";

    return safeString([title UTF8String]);
}

namespace
{
constexpr double minZoomLevel = 0.25;
constexpr double maxZoomLevel = 5.0;
constexpr double zoomStep = 1.1;
} // namespace

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
    detail::applyNativeZoom(impl->webView.get(), clamped, impl->zoomLevel);
}

double WebView::getZoom() const
{
    return detail::readNativeZoom(impl->webView.get(), impl->zoomLevel);
}

WebView* WebView::focused()
{
    return detail::findFocusedWebView();
}

void WebView::evaluateJavaScript(const std::string& script, JSCallback callback)
{
    auto* nsScript = [NSString stringWithUTF8String:script.c_str()];

    if (callback == nullptr)
    {
        [impl->webView.get() evaluateJavaScript:nsScript completionHandler:nil];
        return;
    }

    [impl->webView.get()
        evaluateJavaScript:nsScript
         completionHandler:^(id result, NSError* error) {
           std::string resultStr;
           std::string errorStr;

           if (error != nil)
           {
               errorStr = safeString([error.localizedDescription UTF8String],
                                     "Unknown error");
           }
           else if (result != nil)
           {
               if ([result isKindOfClass:[NSString class]])
               {
                   resultStr = [result UTF8String];
               }
               else if ([result isKindOfClass:[NSNumber class]])
               {
                   resultStr = [[result stringValue] UTF8String];
               }
               else
               {
                   NSError* jsonError = nil;
                   NSData* data =
                       [NSJSONSerialization dataWithJSONObject:result
                                                       options:0
                                                         error:&jsonError];
                   if (data && !jsonError)
                   {
                       resultStr =
                           std::string((const char*) data.bytes, data.length);
                   }
               }
           }

           eacp::Threads::callAsync([callback, resultStr, errorStr]()
                                    { callback(resultStr, errorStr); });
         }];
}

void WebView::takeSnapshot(SnapshotCallback callback)
{
    if (callback == nullptr)
        return;

    detail::takeAppleSnapshot(impl->webView.get(), std::move(callback));
}

void WebView::addScriptMessageHandler(
    const std::string& name, std::function<void(const std::string& message)> handler)
{
    impl->messageHandlers[name] = std::move(handler);

    auto* controller = impl->config.get().userContentController;
    auto* nsName = [NSString stringWithUTF8String:name.c_str()];
    [controller addScriptMessageHandler:impl->delegate.get() name:nsName];
}

void WebView::removeScriptMessageHandler(const std::string& name)
{
    impl->messageHandlers.erase(name);

    auto* controller = impl->config.get().userContentController;
    auto* nsName = [NSString stringWithUTF8String:name.c_str()];
    [controller removeScriptMessageHandlerForName:nsName];
}

void WebView::addUserScript(const std::string& source, bool atDocumentStart)
{
    auto* controller = impl->config.get().userContentController;
    auto injectionTime = atDocumentStart ? WKUserScriptInjectionTimeAtDocumentStart
                                         : WKUserScriptInjectionTimeAtDocumentEnd;

    auto* userScript = [[WKUserScript alloc]
          initWithSource:Strings::toNSString(source)
           injectionTime:injectionTime
        forMainFrameOnly:YES];

    [controller addUserScript:userScript];
}

void WebView::resized()
{
    View::resized();
    impl->updateFrame();
}
} // namespace eacp::Graphics
