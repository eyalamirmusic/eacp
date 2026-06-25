#import <WebKit/WebKit.h>
#import <Foundation/Foundation.h>
#include "Snapshot-Apple.h"
#include "WebView.h"
#include "WebViewPlatform-Apple.h"
#include "StreamingRange.h"

#include <eacp/Core/ObjC/Strings.h>
#include <eacp/Core/Platform/Platform.h>
#include <eacp/Graphics/Primitives/GraphicUtils.h>
#include <eacp/Core/Utils/Containers.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
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

namespace
{
// Per-request streaming state: the resource being pulled, how far we've read,
// how much is left to deliver, and a reusable chunk buffer. The buffer is
// filled on a background queue and consumed on the main thread, but never
// concurrently -- the two hand off across a single chunk.
struct StreamContext
{
    eacp::Graphics::StreamingResource resource;
    eacp::Graphics::RangeSize offset = 0;    // next byte to read
    eacp::Graphics::RangeSize remaining = 0; // bytes still to deliver
    eacp::Graphics::Bytes buffer;
};

constexpr int streamChunkSize = 256 * 1024;
} // namespace

@interface ResourceSchemeHandler : NSObject <WKURLSchemeHandler>
{
@public
    eacp::Graphics::ResourceProvider provider;
    eacp::Graphics::StreamingProvider streamingProvider;
    // Live flags for in-flight streaming tasks, keyed by the task pointer.
    // Touched only on the main thread (start / stop / chunk delivery), so it
    // needs no locking; flipping a flag to false stops further delivery.
    std::unordered_map<const void*, std::shared_ptr<std::atomic<bool>>>
        streamingTasks;
}
@end


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
            schemeHandlers.push_back(handler);
        }

        for (auto& [scheme, streamingProvider]: options.streamingSchemes)
        {
            auto handler = ObjC::Ptr {[[ResourceSchemeHandler alloc] init]};
            handler.get()->streamingProvider = std::move(streamingProvider);
            [config.get() setURLSchemeHandler:handler.get()
                                 forURLScheme:Strings::toNSString(scheme)];
            schemeHandlers.push_back(handler);
        }

        webView = detail::createWebView(
            config.get(),
            detail::WebKitOptions {
              .acceptFirstMouse = options.acceptFirstMouse
            }
        );

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

    ObjC::Ptr<WKWebView> webView;
    ObjC::Ptr<WebViewDelegate> delegate;
    ObjC::Ptr<WKWebViewConfiguration> config;
    Vector<ObjC::Ptr<ResourceSchemeHandler>> schemeHandlers;
    MessageHandlerMap messageHandlers;
    WebView& owner;
    double zoomLevel = 1.0;
    bool observingTitle = false;
};

struct WebViewNativeAccess
{
    static OwningPointer<WebView>
        makePopup(WKWebViewConfiguration* configuration, bool inspectable)
    {
        auto init = WebView::PopupInit {configuration, inspectable};
        return OwningPointer<WebView> {new WebView {init}};
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
            // Guard: dataWithJSONObject: throws (uncaught -> app crash) on a
            // non-array/dict top level, e.g. a bare number posted from JS.
            else if ([NSJSONSerialization isValidJSONObject:message.body])
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

@interface ResourceSchemeHandler ()
- (void)beginStreamingTask:(id<WKURLSchemeTask>)task forURL:(const std::string&)url;
- (void)pumpTask:(id<WKURLSchemeTask>)task
         context:(std::shared_ptr<StreamContext>)context
            live:(std::shared_ptr<std::atomic<bool>>)live;
@end

@implementation ResourceSchemeHandler

- (void)webView:(WKWebView*)webView
    startURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask
{
    auto* url = urlSchemeTask.request.URL.absoluteString;
    auto urlStr = std::string([url UTF8String]);

    if (streamingProvider)
    {
        [self beginStreamingTask:urlSchemeTask forURL:urlStr];
        return;
    }

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
    auto* httpResponse = [[[NSHTTPURLResponse alloc]
        initWithURL:urlSchemeTask.request.URL
         statusCode:response->statusCode
        HTTPVersion:@"HTTP/1.1"
       headerFields:@{@"Content-Type": mime}] autorelease];

    [urlSchemeTask didReceiveResponse:httpResponse];

    auto* data = [NSData dataWithBytes:response->data.data()
                                length:response->data.size()];
    [urlSchemeTask didReceiveData:data];
    [urlSchemeTask didFinish];
}

// Resolves the request's Range header against the resource size, sends the
// matching 200 / 206 / 416 headers, then kicks off the chunked body pump.
- (void)beginStreamingTask:(id<WKURLSchemeTask>)task forURL:(const std::string&)url
{
    auto resource = streamingProvider(url);

    if (!resource)
    {
        auto* error = [NSError errorWithDomain:NSURLErrorDomain
                                          code:NSURLErrorResourceUnavailable
                                      userInfo:nil];
        [task didFailWithError:error];
        return;
    }

    auto rangeHeader = std::string {};

    if (auto* value = [task.request valueForHTTPHeaderField:@"Range"])
        rangeHeader = [value UTF8String];

    auto plan = eacp::Graphics::planStreamingResponse(rangeHeader, *resource);
    auto served = plan.served;

    auto* headers = [NSMutableDictionary dictionary];
    for (const auto& [name, value]: plan.headers)
        headers[eacp::Strings::toNSString(name)] = eacp::Strings::toNSString(value);

    auto* response = [[[NSHTTPURLResponse alloc] initWithURL:task.request.URL
                                                  statusCode:plan.statusCode
                                                 HTTPVersion:@"HTTP/1.1"
                                                headerFields:headers] autorelease];
    [task didReceiveResponse:response];

    if (!plan.hasBody)
    {
        [task didFinish];
        return;
    }

    auto context = std::make_shared<StreamContext>();
    context->resource = std::move(*resource);
    context->offset = served.start;
    context->remaining = served.length;
    context->buffer.resize(streamChunkSize);

    auto live = std::make_shared<std::atomic<bool>>(true);
    streamingTasks[(__bridge const void*) task] = live;

    [self pumpTask:task context:context live:live];
}

// Reads one chunk off a background queue, then hops to the main thread to feed
// it to the task and schedule the next chunk. WKURLSchemeTask methods must run
// on the thread that started the task and must not run after it's stopped, so
// every task call sits behind the main thread plus the `live` guard.
- (void)pumpTask:(id<WKURLSchemeTask>)task
         context:(std::shared_ptr<StreamContext>)context
            live:(std::shared_ptr<std::atomic<bool>>)live
{
    auto* key = (__bridge const void*) task;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      auto want = std::min<eacp::Graphics::RangeSize>(
          static_cast<eacp::Graphics::RangeSize>(context->buffer.size()),
          context->remaining);

      auto got = context->resource.read(
          context->offset,
          eacp::Graphics::ByteSpan {context->buffer.data(),
                                    static_cast<std::size_t>(want)});

      dispatch_async(dispatch_get_main_queue(), ^{
        if (! live->load())
            return;

        if (got == 0)
        {
            [task didFinish];
            self->streamingTasks.erase(key);
            return;
        }

        auto* data = [NSData dataWithBytes:context->buffer.data()
                                    length:static_cast<NSUInteger>(got)];
        [task didReceiveData:data];

        context->offset += got;
        context->remaining -= got;

        if (context->remaining == 0)
        {
            [task didFinish];
            self->streamingTasks.erase(key);
            return;
        }

        [self pumpTask:task context:context live:live];
      });
    });
}

- (void)webView:(WKWebView*)webView
    stopURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask
{
    auto it = streamingTasks.find((__bridge const void*) urlSchemeTask);

    if (it != streamingTasks.end())
    {
        it->second->store(false);
        streamingTasks.erase(it);
    }
}

@end

namespace eacp::Graphics
{
namespace detail
{
WKWebView* wkWebViewOf(WebView* view)
{
    return view != nullptr ? WebViewNativeAccess::wkWebViewOf(*view) : nil;
}
} // namespace detail

void WebView::initNative(Options options)
{
    impl = std::make_shared<Native>(*this, std::move(options));
    impl->delegate.get()->nativeWeak = impl;
    impl->attachToParentView();
    detail::registerWebView(this);

    if (Platform::isMac()) // desktop-only; iOS windows have no chrome to drive
    {
        installWindowDragSupport();
        installWindowControlSupport();
    }
}

WebView::WebView(PopupInit init)
{
    impl = std::make_shared<Native>(*this, init);
    impl->delegate.get()->nativeWeak = impl;
    impl->attachToParentView();
    detail::registerWebView(this);
}

WebView::~WebView()
{
    detail::unregisterWebView(this);
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

void WebView::setZoom(double level)
{
    auto clamped = detail::clampZoom(level);
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

bool WebView::isRuntimeAvailable()
{
    return true;
}

void WebView::evaluateJavaScript(const std::string& script, const JSCallback& callback)
{
    auto* nsScript = [NSString stringWithUTF8String:script.c_str()];

    if (callback == nullptr)
    {
        [impl->webView.get() evaluateJavaScript:nsScript completionHandler:nil];
        return;
    }

    // The block must capture its own copy of the callback: a block captures
    // a C++ reference AS a reference, and `callback` may bind to a caller
    // temporary (callJS passes one) that is gone by the time the async
    // completion handler fires.
    auto ownedCallback = callback;

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

           eacp::Threads::callAsync([ownedCallback, resultStr, errorStr]()
                                    { ownedCallback(resultStr, errorStr); });
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

void WebView::armFileDrag(const Vector<std::string>& paths)
{
    detail::armFileDrag(impl->webView.get(), paths);
}

void WebView::armWindowDrag()
{
    detail::armWindowDrag(impl->webView.get());
}

void WebView::performWindowControl(const std::string& action)
{
    detail::performWindowControl(impl->webView.get(), action);
}

void WebView::resized()
{
    View::resized();
    impl->updateFrame();
}

// The native WKWebView is a real subview that receives input directly, so the
// framework never routes these to us; they exist only to satisfy the shared
// declaration the Windows composition-hosted backend needs.
void WebView::mouseDown(const MouseEvent&) {}
void WebView::mouseUp(const MouseEvent&) {}
void WebView::mouseDragged(const MouseEvent&) {}
void WebView::mouseMoved(const MouseEvent&) {}
void WebView::mouseExited(const MouseEvent&) {}
void WebView::mouseWheel(const MouseEvent&) {}
} // namespace eacp::Graphics
