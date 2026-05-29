#import <WebKit/WebKit.h>
#import <Foundation/Foundation.h>
#include "Snapshot-Apple.h"
#include "WebView.h"
#include "WebViewPlatform-Apple.h"

#include <eacp/Core/ObjC/Strings.h>
#include <eacp/Graphics/Primitives/GraphicUtils.h>
#include <ea_data_structures/Structures/Vector.h>
#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace
{
std::string safeString(const char* str, const char* fallback = "")
{
    return str != nullptr ? str : fallback;
}

std::optional<std::size_t> parseSize(std::string_view value)
{
    if (value.empty())
        return std::nullopt;

    auto out = std::size_t {};

    for (auto c: value)
    {
        if (c < '0' || c > '9')
            return std::nullopt;

        auto digit = static_cast<std::size_t>(c - '0');

        if (out > (std::numeric_limits<std::size_t>::max() - digit) / 10)
            return std::nullopt;

        out = out * 10 + digit;
    }

    return out;
}

// Parses an HTTP `Range` header against a known total size, writing the
// resolved inclusive [start, end] byte offsets. Handles `bytes=a-b`,
// `bytes=a-` and suffix `bytes=-n`. Returns false (serve the whole body)
// for anything malformed, unsatisfiable, or multi-range.
bool parseByteRange(const std::string& header,
                    std::size_t total,
                    std::size_t& start,
                    std::size_t& end)
{
    constexpr std::string_view prefix = "bytes=";

    if (total == 0 || header.rfind(prefix, 0) != 0
        || header.find(',') != std::string::npos)
        return false;

    auto spec = header.substr(prefix.size());
    auto dash = spec.find('-');

    if (dash == std::string::npos)
        return false;

    auto firstStr = spec.substr(0, dash);
    auto lastStr = spec.substr(dash + 1);

    if (firstStr.empty())
    {
        auto suffix = parseSize(lastStr);

        if (!suffix || *suffix == 0)
            return false;

        auto clampedSuffix = std::min(*suffix, total);
        start = total - clampedSuffix;
        end = total - 1;
    }
    else
    {
        auto first = parseSize(firstStr);
        auto last = lastStr.empty() ? std::optional<std::size_t> {total - 1}
                                    : parseSize(lastStr);

        if (!first || !last)
            return false;

        start = *first;
        end = *last;
    }

    if (start > end || start >= total)
        return false;

    end = std::min(end, total - 1);
    return true;
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

// Streams on-disk files into the WebView. Each task resolves a URL to a path,
// then reads ONLY the requested byte range on a background queue and delivers
// it back on the main thread -- so a large media file neither blocks the UI
// nor gets re-read whole on every range request the media engine issues.
@interface FileSchemeHandler : NSObject <WKURLSchemeHandler>
{
@public
    eacp::Graphics::FilePathResolver resolver;
}
// Tasks still in flight, touched only on the main thread. A task is dropped
// on stop / completion; delivery is skipped if its task is no longer here,
// which is how we avoid messaging a stopped task (WKWebView throws if we do).
@property(strong) NSMutableSet* liveTasks;
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
            schemeHandlers.push_back(std::move(handler));
        }

        for (auto& [scheme, resolver]: options.fileSchemes)
        {
            auto handler = ObjC::Ptr {[[FileSchemeHandler alloc] init]};
            handler.get()->resolver = std::move(resolver);
            [config.get() setURLSchemeHandler:handler.get()
                                 forURLScheme:Strings::toNSString(scheme)];
            fileSchemeHandlers.push_back(std::move(handler));
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
    EA::Vector<ObjC::Ptr<ResourceSchemeHandler>> schemeHandlers;
    EA::Vector<ObjC::Ptr<FileSchemeHandler>> fileSchemeHandlers;
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
    auto* request = urlSchemeTask.request;
    auto urlStr = std::string([request.URL.absoluteString UTF8String]);

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

    auto total = static_cast<std::size_t>(response->data.size());
    auto start = std::size_t {0};
    auto end = total > 0 ? total - 1 : std::size_t {0};

    // Honour a byte-range request so media elements can seek -- WKWebView's
    // media engine probes a custom scheme with `Range:` and expects a 206.
    auto* rangeHeader = [request valueForHTTPHeaderField:@"Range"];
    auto isRange = rangeHeader != nil
                && parseByteRange([rangeHeader UTF8String], total, start, end);
    auto length = isRange ? end - start + 1 : total;

    auto* headers = [NSMutableDictionary dictionary];
    headers[@"Content-Type"] = eacp::Strings::toNSString(response->mimeType);
    headers[@"Accept-Ranges"] = @"bytes";
    headers[@"Content-Length"] =
        [NSString stringWithFormat:@"%llu", (unsigned long long) length];

    auto status = response->statusCode;

    if (isRange)
    {
        status = 206;
        headers[@"Content-Range"] = [NSString
            stringWithFormat:@"bytes %llu-%llu/%llu", (unsigned long long) start,
                             (unsigned long long) end, (unsigned long long) total];
    }

    auto* httpResponse =
        [[[NSHTTPURLResponse alloc] initWithURL:request.URL
                                     statusCode:status
                                    HTTPVersion:@"HTTP/1.1"
                                   headerFields:headers] autorelease];

    [urlSchemeTask didReceiveResponse:httpResponse];

    auto* data = [NSData dataWithBytes:length > 0 ? response->data.data() + start
                                               : nullptr
                                length:length];
    [urlSchemeTask didReceiveData:data];
    [urlSchemeTask didFinish];
}

- (void)webView:(WKWebView*)webView
    stopURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask
{
}

@end

@implementation FileSchemeHandler

- (instancetype)init
{
    self = [super init];

    if (self != nil)
    {
        // Owned (alloc/init): the WebView runs under manual reference counting,
        // so an autoreleased set could be gone before the first task arrives.
        _liveTasks = [[NSMutableSet alloc] init];
    }

    return self;
}

- (void)dealloc
{
    [_liveTasks release];
    [super dealloc];
}

- (void)webView:(WKWebView*)webView
    startURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask
{
    auto* request = urlSchemeTask.request;
    auto* requestURL = request.URL;
    auto urlStr = std::string([requestURL.absoluteString UTF8String]);

    auto* rangeNS = [request valueForHTTPHeaderField:@"Range"];
    auto rangeStr =
        rangeNS != nil ? std::string([rangeNS UTF8String]) : std::string {};

    auto resolveFn = self->resolver;

    // Tracked on the main thread so a stop (below) before delivery cancels it.
    [self.liveTasks addObject:urlSchemeTask];

    // The disk I/O happens off the main thread; only the requested byte range
    // is read, never the whole file.
    auto* ioQueue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_async(ioQueue, ^{
      auto resolved = resolveFn ? resolveFn(urlStr) : std::nullopt;

      NSHTTPURLResponse* httpResponse = nil;
      NSData* data = nil;

      if (resolved)
      {
          auto* path = [NSString stringWithUTF8String:resolved->c_str()];
          auto* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path
                                                                         error:nil];
          auto* handle = [NSFileHandle fileHandleForReadingAtPath:path];

          if (attrs != nil && handle != nil)
          {
              auto total = static_cast<std::size_t>([attrs fileSize]);
              auto start = std::size_t {0};
              auto end = total > 0 ? total - 1 : std::size_t {0};
              auto isRange = !rangeStr.empty()
                          && parseByteRange(rangeStr, total, start, end);
              auto length = isRange ? end - start + 1 : total;

              auto ok = start == 0 || [handle seekToOffset:start error:nil];

              if (ok)
                  data = length > 0 ? [handle readDataUpToLength:length error:nil]
                                    : [NSData data];

              [handle closeAndReturnError:nil];

              if (data != nil && [data length] == length)
              {
                  auto* headers = [NSMutableDictionary dictionary];
                  headers[@"Content-Type"] = eacp::Strings::toNSString(
                      eacp::Graphics::mimeForPath(resolved.value()));
                  headers[@"Accept-Ranges"] = @"bytes";
                  headers[@"Content-Length"] =
                      [NSString stringWithFormat:@"%llu", (unsigned long long) length];

                  auto status = 200;

                  if (isRange)
                  {
                      status = 206;
                      headers[@"Content-Range"] =
                          [NSString stringWithFormat:@"bytes %llu-%llu/%llu",
                                                     (unsigned long long) start,
                                                     (unsigned long long) end,
                                                     (unsigned long long) total];
                  }

                  // Autoreleased: the main-queue block below retains it while
                  // this block's pool still holds it, so it survives the hop.
                  httpResponse =
                      [[[NSHTTPURLResponse alloc] initWithURL:requestURL
                                                   statusCode:status
                                                  HTTPVersion:@"HTTP/1.1"
                                                 headerFields:headers] autorelease];
              }
          }
      }

      dispatch_async(dispatch_get_main_queue(), ^{
        if (![self.liveTasks containsObject:urlSchemeTask])
            return;

        if (httpResponse != nil && data != nil)
        {
            [urlSchemeTask didReceiveResponse:httpResponse];
            [urlSchemeTask didReceiveData:data];
            [urlSchemeTask didFinish];
        }
        else
        {
            auto* error = [NSError errorWithDomain:NSURLErrorDomain
                                              code:NSURLErrorResourceUnavailable
                                          userInfo:nil];
            [urlSchemeTask didFailWithError:error];
        }

        [self.liveTasks removeObject:urlSchemeTask];
      });
    });
}

- (void)webView:(WKWebView*)webView
    stopURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask
{
    [self.liveTasks removeObject:urlSchemeTask];
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

void WebView::armFileDrag(const std::vector<std::string>& paths)
{
    detail::armFileDrag(impl->webView.get(), paths);
}

void WebView::resized()
{
    View::resized();
    impl->updateFrame();
}
} // namespace eacp::Graphics
