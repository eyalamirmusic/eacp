#import <WebKit/WebKit.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#include "Snapshot-Apple.h"
#include "WebView.h"
#include "WebViewPlatform-Apple.h"
#include "StreamingRange.h"
#include <algorithm>

#include <eacp/Core/ObjC/RuntimeClass.h>
#include <eacp/Core/ObjC/Strings.h>
#include <eacp/Core/Threads/ThreadUtils.h>
#include <eacp/Graphics/Primitives/GraphicUtils.h>
#if !TARGET_OS_IPHONE
#include <eacp/Graphics/Graphics/Keyboard-MacOS.h>
#endif
#include <atomic>

namespace
{
std::string safeString(const char* str, const char* fallback = "")
{
    return str != nullptr ? str : fallback;
}
} // namespace

#if EACP_WEBVIEW_PRIVATE_MEDIA_CAPTURE_SPI
// Private WebKit SPI: macOS 11 has no public camera/microphone permission
// selector and calls this underscored variant instead. The bit values are ABI.
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

namespace
{
// Runtime classes construct no C++ ivars, so each instance's C++ members live
// behind one raw "state" pointer, created alongside it and deleted in dealloc.
struct WebViewDelegateState
{
    std::weak_ptr<eacp::Graphics::WebView::Native> nativeWeak;
    MessageHandlerMap* messageHandlers = nullptr;
};

WebViewDelegateState* getWebViewDelegateState(id self)
{
    return (WebViewDelegateState*) eacp::ObjC::getIvar<void*>(self, "state");
}

// Returns a +1 instance with a fresh state; defined below WebView::Native,
// whose complete type the delegate's methods need.
NSObject* createWebViewDelegate();

// The buffer is filled on a background queue and consumed on the main thread,
// but never concurrently — the two hand off across a single chunk.
struct StreamContext
{
    eacp::Graphics::StreamingResource resource;
    eacp::Graphics::RangeSize offset = 0;    // next byte to read
    eacp::Graphics::RangeSize remaining = 0; // bytes still to deliver
    eacp::Graphics::Bytes buffer;
};

constexpr int streamChunkSize = 256 * 1024;

struct ResourceSchemeHandlerState
{
    eacp::Graphics::ResourceProvider provider;
    eacp::Graphics::StreamingProvider streamingProvider;

    // Live flags for in-flight streaming tasks, keyed by task pointer. Main
    // thread only, so unlocked; clearing a flag stops further delivery.
    std::unordered_map<const void*, std::shared_ptr<std::atomic<bool>>>
        streamingTasks;
};

ResourceSchemeHandlerState* getSchemeHandlerState(id self)
{
    return (ResourceSchemeHandlerState*) eacp::ObjC::getIvar<void*>(self,
                                                                    "state");
}

NSObject* createResourceSchemeHandler();
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
        delegate = createWebViewDelegate();
        getWebViewDelegateState(delegate.get())->messageHandlers =
            &messageHandlers;

        config = [[WKWebViewConfiguration alloc] init];

        if (options.debugConsole)
        {
            [config.get().preferences setValue:@YES
                                        forKey:@"developerExtrasEnabled"];
        }

        for (auto& [scheme, provider]: options.schemes)
        {
            auto handler = ObjC::Ptr {createResourceSchemeHandler()};
            getSchemeHandlerState(handler.get())->provider = std::move(provider);
            [config.get()
                setURLSchemeHandler:(id<WKURLSchemeHandler>) handler.get()
                       forURLScheme:Strings::toNSString(scheme)];
            schemeHandlers.push_back(handler);
        }

        for (auto& [scheme, streamingProvider]: options.streamingSchemes)
        {
            auto handler = ObjC::Ptr {createResourceSchemeHandler()};
            getSchemeHandlerState(handler.get())->streamingProvider =
                std::move(streamingProvider);
            [config.get()
                setURLSchemeHandler:(id<WKURLSchemeHandler>) handler.get()
                       forURLScheme:Strings::toNSString(scheme)];
            schemeHandlers.push_back(handler);
        }

        webView = detail::createWebView(
            config.get(),
            detail::WebKitOptions {
              .acceptFirstMouse = options.acceptFirstMouse
            }
        );
        if (options.transparentBackground)
        {
#if TARGET_OS_IPHONE
            webView.get().opaque = NO;
            webView.get().backgroundColor = UIColor.clearColor;
            webView.get().scrollView.backgroundColor = UIColor.clearColor;
#else
            [webView.get() setValue:@NO forKey:@"drawsBackground"];
            webView.get().wantsLayer = YES;
            webView.get().layer.backgroundColor = NSColor.clearColor.CGColor;
            if (@available(macOS 12.0, *))
                webView.get().underPageBackgroundColor = NSColor.clearColor;
#endif
        }
        detail::setFileDragStartedCallback(
            webView.get(),
            [this] { owner.onFileDragStarted(); });

        webView.get().navigationDelegate =
            (id<WKNavigationDelegate>) delegate.get();
        webView.get().UIDelegate = (id<WKUIDelegate>) delegate.get();

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
        delegate = createWebViewDelegate();
        getWebViewDelegateState(delegate.get())->messageHandlers =
            &messageHandlers;

        config = ObjC::Ptr {init.configuration, ObjC::RetainMode {}};

        auto rect = CGRectMake(0, 0, 100, 100);
        webView = [[WKWebView alloc] initWithFrame:rect configuration:config.get()];
        webView.get().navigationDelegate =
            (id<WKNavigationDelegate>) delegate.get();
        webView.get().UIDelegate = (id<WKUIDelegate>) delegate.get();

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
    ObjC::Ptr<NSObject> delegate;
    ObjC::Ptr<WKWebViewConfiguration> config;
    Vector<ObjC::Ptr<NSObject>> schemeHandlers;
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

namespace
{
void webViewDelegateDidStartNavigation(id self,
                                       SEL,
                                       WKWebView* webView,
                                       WKNavigation*)
{
    auto url = safeString([webView.URL.absoluteString UTF8String]);
    eacp::Threads::callAsync(
        [weak = getWebViewDelegateState(self)->nativeWeak, url]()
        {
            if (auto native = weak.lock())
                native->owner.onNavigationStarted(url);
        });
}

void webViewDelegateDidFinishNavigation(id self,
                                        SEL,
                                        WKWebView* webView,
                                        WKNavigation*)
{
    auto url = safeString([webView.URL.absoluteString UTF8String]);
    eacp::Threads::callAsync(
        [weak = getWebViewDelegateState(self)->nativeWeak, url]()
        {
            if (auto native = weak.lock())
                native->owner.onNavigationFinished(url);
        });
}

void webViewDelegateDidFailProvisionalNavigation(
    id self, SEL, WKWebView*, WKNavigation*, NSError* error)
{
    auto errorStr =
        safeString([error.localizedDescription UTF8String], "Unknown error");
    eacp::Threads::callAsync(
        [weak = getWebViewDelegateState(self)->nativeWeak, errorStr]()
        {
            if (auto native = weak.lock())
                native->owner.onNavigationFailed(errorStr);
        });
}

void webViewDelegateDidFailNavigation(
    id self, SEL, WKWebView*, WKNavigation*, NSError* error)
{
    auto errorStr =
        safeString([error.localizedDescription UTF8String], "Unknown error");
    eacp::Threads::callAsync(
        [weak = getWebViewDelegateState(self)->nativeWeak, errorStr]()
        {
            if (auto native = weak.lock())
                native->owner.onNavigationFailed(errorStr);
        });
}

void webViewDelegateObserveValue(id self,
                                 SEL,
                                 NSString* keyPath,
                                 id object,
                                 NSDictionary<NSKeyValueChangeKey, id>*,
                                 void*)
{
    if (! [keyPath isEqualToString:@"title"])
        return;

    auto* webView = (WKWebView*) object;
    auto title = safeString([webView.title UTF8String]);
    eacp::Threads::callAsync(
        [weak = getWebViewDelegateState(self)->nativeWeak, title]()
        {
            if (auto native = weak.lock())
                native->owner.onTitleChanged(title);
        });
}

WKWebView* webViewDelegateCreateWebView(id self,
                                        SEL,
                                        WKWebView* webView,
                                        WKWebViewConfiguration* configuration,
                                        WKNavigationAction* navigationAction,
                                        WKWindowFeatures*)
{
    auto url = safeString([navigationAction.request.URL.absoluteString UTF8String]);

    auto inspectable = NO;
    if (@available(macOS 13.3, iOS 16.4, *))
        inspectable = webView.inspectable;

    auto native = getWebViewDelegateState(self)->nativeWeak.lock();
    if (! native)
        return nil;

    auto popup = eacp::Graphics::WebViewNativeAccess::makePopup(configuration,
                                                                inspectable);
    auto* popupWKWebView = eacp::Graphics::WebViewNativeAccess::wkWebViewOf(*popup);

    if (native->owner.onNewWindowRequested(std::move(popup), url))
        return popupWKWebView;

    // Embedder declined, so load inline rather than dropping the navigation.
    if (navigationAction.targetFrame == nil)
        [webView loadRequest:navigationAction.request];

    return nil;
}

void webViewDelegateDidClose(id self, SEL, WKWebView*)
{
    eacp::Threads::callAsync(
        [weak = getWebViewDelegateState(self)->nativeWeak]()
        {
            if (auto native = weak.lock())
                native->owner.onClose();
        });
}

API_AVAILABLE(macos(12.0), ios(15.0))
void webViewDelegateRequestMediaCapture(
    id,
    SEL,
    WKWebView*,
    WKSecurityOrigin*,
    WKFrameInfo*,
    WKMediaCaptureType,
    void (^handler)(WKPermissionDecision decision))
{
    handler(WKPermissionDecisionGrant);
}

#if EACP_WEBVIEW_PRIVATE_MEDIA_CAPTURE_SPI
// macOS 11 fallback for the public selector above. The availability guard keeps
// the public path winning if a future WebKit ever dispatched both. handler() is
// always invoked so the request cannot hang.
void webViewDelegateRequestUserMediaSPI(id,
                                        SEL,
                                        WKWebView*,
                                        _WKCaptureDevices,
                                        NSURL*,
                                        NSURL*,
                                        void (^handler)(BOOL authorized))
{
    if (@available(macOS 12.0, *))
    {
        handler(NO);
        return;
    }
    handler(YES);
}
#endif

void webViewDelegateDidReceiveScriptMessage(id self,
                                            SEL,
                                            WKUserContentController*,
                                            WKScriptMessage* message)
{
    auto* messageHandlers = getWebViewDelegateState(self)->messageHandlers;

    if (messageHandlers)
    {
        auto name = std::string([message.name UTF8String]);
        auto it = messageHandlers->find(name);
        if (it != messageHandlers->end())
        {
            std::string body;
            if ([message.body isKindOfClass:[NSString class]])
            {
                body = [message.body UTF8String];
            }
            // dataWithJSONObject: throws, uncaught, on a non-array/dict top
            // level — e.g. a bare number posted from JS.
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

void deallocWebViewDelegate(id self, SEL)
{
    delete getWebViewDelegateState(self);
    eacp::ObjC::sendSuper<void>(self, [NSObject class], @selector(dealloc));
}

Class getWebViewDelegateClass()
{
    static auto instance = []
    {
        auto builder =
            new eacp::ObjC::RuntimeClass<NSObject>("EacpWebViewDelegate");

        builder->addIvar<void*>("state");
        builder->addProtocol(@protocol(WKNavigationDelegate));
        builder->addProtocol(@protocol(WKScriptMessageHandler));
        builder->addProtocol(@protocol(WKUIDelegate));

        builder->addMethod(@selector(webView:didStartProvisionalNavigation:),
                           webViewDelegateDidStartNavigation);
        builder->addMethod(@selector(webView:didFinishNavigation:),
                           webViewDelegateDidFinishNavigation);
        builder->addMethod(
            @selector(webView:didFailProvisionalNavigation:withError:),
            webViewDelegateDidFailProvisionalNavigation);
        builder->addMethod(@selector(webView:didFailNavigation:withError:),
                           webViewDelegateDidFailNavigation);
        builder->addMethod(
            @selector(observeValueForKeyPath:ofObject:change:context:),
            webViewDelegateObserveValue);
        builder->addMethod(
            @selector(webView:
                createWebViewWithConfiguration:forNavigationAction:windowFeatures:),
            webViewDelegateCreateWebView);
        builder->addMethod(@selector(webViewDidClose:), webViewDelegateDidClose);

        // The types in this selector's signature only exist on macOS 12 /
        // iOS 15+; older systems take the SPI fallback below.
        if (@available(macOS 12.0, iOS 15.0, *))
            builder->addMethod(
                @selector(webView:
                    requestMediaCapturePermissionForOrigin:initiatedByFrame:type
                                                          :decisionHandler:),
                webViewDelegateRequestMediaCapture);

#if EACP_WEBVIEW_PRIVATE_MEDIA_CAPTURE_SPI
        builder->addMethod(
            @selector(_webView:
                requestUserMediaAuthorizationForDevices:url:mainFrameURL
                                                       :decisionHandler:),
            webViewDelegateRequestUserMediaSPI);
#endif

        builder->addMethod(
            @selector(userContentController:didReceiveScriptMessage:),
            webViewDelegateDidReceiveScriptMessage);
        builder->addMethod(@selector(dealloc), deallocWebViewDelegate);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

NSObject* createWebViewDelegate()
{
    NSObject* delegate = [[getWebViewDelegateClass() alloc] init];
    eacp::ObjC::getIvar<void*>(delegate, "state") = new WebViewDelegateState();
    return delegate;
}
} // namespace

namespace
{
// WKURLSchemeTask methods must run on the thread that started the task and
// never after it stops, hence the main-thread hop and the `live` guard.
void schemeHandlerPumpTask(id self,
                           id<WKURLSchemeTask> task,
                           std::shared_ptr<StreamContext> context,
                           std::shared_ptr<std::atomic<bool>> live)
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
            getSchemeHandlerState(self)->streamingTasks.erase(key);
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
            getSchemeHandlerState(self)->streamingTasks.erase(key);
            return;
        }

        schemeHandlerPumpTask(self, task, context, live);
      });
    });
}

void schemeHandlerBeginStreamingTask(id self,
                                     id<WKURLSchemeTask> task,
                                     const std::string& url)
{
    auto resource = getSchemeHandlerState(self)->streamingProvider(url);

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
    getSchemeHandlerState(self)->streamingTasks[(__bridge const void*) task] =
        live;

    schemeHandlerPumpTask(self, task, context, live);
}

void schemeHandlerStartTask(id self,
                            SEL,
                            WKWebView*,
                            id<WKURLSchemeTask> urlSchemeTask)
{
    auto* url = urlSchemeTask.request.URL.absoluteString;
    auto urlStr = std::string([url UTF8String]);

    auto* state = getSchemeHandlerState(self);

    if (state->streamingProvider)
    {
        schemeHandlerBeginStreamingTask(self, urlSchemeTask, urlStr);
        return;
    }

    auto response = state->provider ? state->provider(urlStr) : std::nullopt;

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

void schemeHandlerStopTask(id self,
                           SEL,
                           WKWebView*,
                           id<WKURLSchemeTask> urlSchemeTask)
{
    auto& streamingTasks = getSchemeHandlerState(self)->streamingTasks;
    auto it = streamingTasks.find((__bridge const void*) urlSchemeTask);

    if (it != streamingTasks.end())
    {
        it->second->store(false);
        streamingTasks.erase(it);
    }
}

void deallocSchemeHandler(id self, SEL)
{
    delete getSchemeHandlerState(self);
    eacp::ObjC::sendSuper<void>(self, [NSObject class], @selector(dealloc));
}

Class getResourceSchemeHandlerClass()
{
    static auto instance = []
    {
        auto builder =
            new eacp::ObjC::RuntimeClass<NSObject>("EacpResourceSchemeHandler");

        builder->addIvar<void*>("state");
        builder->addProtocol(@protocol(WKURLSchemeHandler));

        builder->addMethod(@selector(webView:startURLSchemeTask:),
                           schemeHandlerStartTask);
        builder->addMethod(@selector(webView:stopURLSchemeTask:),
                           schemeHandlerStopTask);
        builder->addMethod(@selector(dealloc), deallocSchemeHandler);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

NSObject* createResourceSchemeHandler()
{
    NSObject* handler = [[getResourceSchemeHandlerClass() alloc] init];
    eacp::ObjC::getIvar<void*>(handler, "state") =
        new ResourceSchemeHandlerState();
    return handler;
}
} // namespace

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
    auto forwardKeys = options.forwardUnhandledKeys;
    auto driveOffscreen = options.driveOffscreenAnimation;

    impl = std::make_shared<Native>(*this, std::move(options));
    getWebViewDelegateState(impl->delegate.get())->nativeWeak = impl;
    impl->attachToParentView();
    detail::registerWebView(this);

    if (Platform::isMac()) // desktop-only; iOS windows have no chrome to drive
    {
        installWindowDragSupport();
        installWindowControlSupport();

        if (forwardKeys)
            installKeyEventSupport();
    }

    if (driveOffscreen)
        installOffscreenAnimationSupport();
}

WebView::WebView(PopupInit init)
{
    impl = std::make_shared<Native>(*this, init);
    getWebViewDelegateState(impl->delegate.get())->nativeWeak = impl;
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

void WebView::focusContent()
{
#if TARGET_OS_IPHONE
    [impl->webView.get() becomeFirstResponder];
#else
    [impl->webView.get().window makeFirstResponder:impl->webView.get()];
#endif
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
    Threads::assertMainThread();

    auto* nsScript = [NSString stringWithUTF8String:script.c_str()];

    if (callback == nullptr)
    {
        [impl->webView.get() evaluateJavaScript:nsScript completionHandler:nil];
        return;
    }

    // Blocks capture a C++ reference as a reference, and `callback` may bind
    // to a caller temporary that dies before the completion handler fires.
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
    [controller
        addScriptMessageHandler:(id<WKScriptMessageHandler>) impl->delegate.get()
                           name:nsName];
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

// Keys the page leaves unconsumed go to onUnhandledKeyEvent, then, unless it
// claims them, up the responder chain PAST the framework container view.
void WebView::installKeyEventSupport()
{
#if !TARGET_OS_IPHONE
    auto shim = ResEmbed::get("key-events.js", "EacpWebView");
    if (!shim)
        throw std::runtime_error(
            "eacp-webview: embedded key-events.js resource not found");

    addUserScript(shim.toString(), true);

    // The page posts "<down|up>:<0|1>:...", 1 meaning it consumed the key. The
    // trailing fields are for Windows, which has no native event stash.
    addScriptMessageHandler("__eacpKeyEvent",
                            [this](const std::string& message)
                            {
                                auto first = message.find(':');
                                if (first == std::string::npos)
                                    return;

                                auto isDown =
                                    message.compare(0, first, "down") == 0;

                                auto second = message.find(':', first + 1);
                                auto count = second == std::string::npos
                                                 ? std::string::npos
                                                 : second - (first + 1);
                                auto consumed =
                                    message.substr(first + 1, count) == "1";

                                detail::reportKeyVerdict(impl->webView.get(),
                                                         isDown,
                                                         consumed);
                            });

    detail::setUnhandledKeyCallback(
        impl->webView.get(),
        [this](NSEvent* event, bool isDown)
        {
            auto type = isDown ? KeyEventType::Down : KeyEventType::Up;

            if (onUnhandledKeyEvent && onUnhandledKeyEvent(keyEventFrom(event, type)))
                return;

            auto* webView = impl->webView.get();
            NSResponder* next = webView.superview.nextResponder;
            if (next == nil)
                return;

            if (isDown)
                [next keyDown:event];
            else
                [next keyUp:event];
        });
#endif
}

void WebView::resized()
{
    View::resized();
    impl->updateFrame();
}

void* WebView::nativeFocusTarget()
{
    if (impl != nullptr && impl->webView.get() != nil)
        return impl->webView.get();

    return View::nativeFocusTarget();
}

// The WKWebView is a real subview and receives input directly, so these exist
// only for the composition-hosted Windows backend's sake.
void WebView::mouseDown(const MouseEvent&) {}
void WebView::mouseUp(const MouseEvent&) {}
void WebView::mouseDragged(const MouseEvent&) {}
void WebView::mouseMoved(const MouseEvent&) {}
void WebView::mouseExited(const MouseEvent&) {}
void WebView::mouseWheel(const MouseEvent&) {}

// An ordinary subview follows the window's placement and visibility already.
void WebView::hostWindowMoved() {}
void WebView::hostWindowVisibilityChanged(bool) {}
} // namespace eacp::Graphics
