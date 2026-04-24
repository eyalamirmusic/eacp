#import <WebKit/WebKit.h>
#import <Foundation/Foundation.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif
#include "WebView.h"

#include <eacp/Core/ObjC/Strings.h>
#include <eacp/Graphics/Primitives/GraphicUtils.h>
#include <unordered_map>

namespace
{
std::string safeString(const char* str, const char* fallback = "")
{
    return str != nullptr ? str : fallback;
}
} // namespace

namespace eacp::Graphics
{
class WebView;
}

using MessageHandlerMap =
    std::unordered_map<std::string, std::function<void(const std::string&)>>;

@interface WebViewDelegate : NSObject <WKNavigationDelegate, WKScriptMessageHandler>
@property(assign) eacp::Graphics::WebView* owner;
@property(assign) MessageHandlerMap* messageHandlers;
@end

@interface ResourceSchemeHandler : NSObject <WKURLSchemeHandler>
{
@public
    eacp::Graphics::ResourceProvider provider;
}
@end

namespace eacp::Graphics
{
struct WebView::Native
{
    Native(WebView& ownerToUse, Options options)
        : owner(ownerToUse)
    {
        delegate = [[WebViewDelegate alloc] init];
        delegate.get().owner = &owner;
        delegate.get().messageHandlers = &messageHandlers;

        config = [[WKWebViewConfiguration alloc] init];

        for (auto& [scheme, provider]: options.schemes)
        {
            auto handler = ObjC::Ptr {[[ResourceSchemeHandler alloc] init]};
            handler.get()->provider = std::move(provider);
            [config.get() setURLSchemeHandler:handler.get()
                                 forURLScheme:Strings::toNSString(scheme)];
            schemeHandlers.push_back(std::move(handler));
        }

        auto rect = CGRectMake(0, 0, 100, 100);
        webView = [[WKWebView alloc] initWithFrame:rect configuration:config.get()];
        webView.get().navigationDelegate = delegate.get();
    }
    ~Native()
    {
        auto controller = config.get().userContentController;

        for (auto& [name, _]: messageHandlers)
        {
            [controller removeScriptMessageHandlerForName:Strings::toNSString(name)];
        }

        webView.get().navigationDelegate = nil;
    }

    void attachToParentView()
    {
        auto* parentHandle = owner.getHandle();

        if (parentHandle != nullptr)
        {
#if TARGET_OS_IPHONE
            auto* parentView = (__bridge UIView*) parentHandle;
#else
            auto* parentView = (__bridge NSView*) parentHandle;
#endif
            [parentView addSubview:webView.get()];
        }
    }
    void updateFrame()
    {
        auto bounds = owner.getLocalBounds();
        webView.get().frame = toCGRect(bounds);
    }

    ObjC::Ptr<WKWebView> webView;
    ObjC::Ptr<WebViewDelegate> delegate;
    ObjC::Ptr<WKWebViewConfiguration> config;
    std::vector<ObjC::Ptr<ResourceSchemeHandler>> schemeHandlers;
    MessageHandlerMap messageHandlers;
    WebView& owner;
};

} // namespace eacp::Graphics

@implementation WebViewDelegate

- (void)webView:(WKWebView*)webView
    didStartProvisionalNavigation:(WKNavigation*)navigation
{
    if (_owner->onNavigationStarted)
    {
        auto url = safeString([webView.URL.absoluteString UTF8String]);
        eacp::Threads::callAsync(
            [owner = _owner, url]()
            {
                if (owner->onNavigationStarted)
                    owner->onNavigationStarted(url);
            });
    }
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation
{
    if (_owner->onNavigationFinished)
    {
        auto url = safeString([webView.URL.absoluteString UTF8String]);
        eacp::Threads::callAsync(
            [owner = _owner, url]()
            {
                if (owner->onNavigationFinished)
                    owner->onNavigationFinished(url);
            });
    }
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error
{
    if (_owner->onNavigationFailed)
    {
        auto errorStr =
            safeString([error.localizedDescription UTF8String], "Unknown error");
        eacp::Threads::callAsync(
            [owner = _owner, errorStr]()
            {
                if (owner->onNavigationFailed)
                    owner->onNavigationFailed(errorStr);
            });
    }
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
            withError:(NSError*)error
{
    if (_owner && _owner->onNavigationFailed)
    {
        auto errorStr =
            safeString([error.localizedDescription UTF8String], "Unknown error");
        eacp::Threads::callAsync(
            [owner = _owner, errorStr]()
            {
                if (owner->onNavigationFailed)
                    owner->onNavigationFailed(errorStr);
            });
    }
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context
{
    if ([keyPath isEqualToString:@"title"] && _owner && _owner->onTitleChanged)
    {
        auto* webView = (WKWebView*) object;
        auto title = safeString([webView.title UTF8String]);
        eacp::Threads::callAsync(
            [owner = _owner, title]()
            {
                if (owner->onTitleChanged)
                    owner->onTitleChanged(title);
            });
    }
}

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
std::string mimeForPath(std::string_view path)
{
    if (path.ends_with(".html"))
        return "text/html; charset=utf-8";
    if (path.ends_with(".js"))
        return "application/javascript; charset=utf-8";
    if (path.ends_with(".css"))
        return "text/css; charset=utf-8";
    if (path.ends_with(".json"))
        return "application/json; charset=utf-8";
    if (path.ends_with(".svg"))
        return "image/svg+xml";
    if (path.ends_with(".png"))
        return "image/png";
    if (path.ends_with(".jpg") || path.ends_with(".jpeg"))
        return "image/jpeg";
    if (path.ends_with(".woff2"))
        return "font/woff2";
    return "application/octet-stream";
}

std::string pathFromURL(std::string_view url, std::string_view indexFile)
{
    auto schemeEnd = url.find("://");

    if (schemeEnd == std::string_view::npos)
        return {};

    auto afterHost = url.find('/', schemeEnd + 3);

    if (afterHost == std::string_view::npos)
        return std::string(indexFile);

    auto path = url.substr(afterHost + 1);
    auto query = path.find('?');

    if (query != std::string_view::npos)
        path = path.substr(0, query);

    return path.empty() ? std::string(indexFile) : std::string(path);
}

FileProvider fromResEmbed(std::string category)
{
    return [category = std::move(category)](std::string_view path)
        -> std::optional<std::span<const std::uint8_t>>
    {
        auto view = ResEmbed::get(std::string(path), category);

        if (!view)
            return std::nullopt;

        return std::span<const std::uint8_t> {view.data(), view.size()};
    };
}

namespace
{
ResourceProvider makeResourceProviderFromFiles(FileProvider provider,
                                               std::string indexFile)
{
    return [provider = std::move(provider),
            indexFile = std::move(indexFile)](std::string_view url)
        -> std::optional<ResourceResponse>
    {
        auto path = pathFromURL(url, indexFile);
        auto bytes = provider ? provider(path) : std::nullopt;

        if (!bytes)
            return std::nullopt;

        ResourceResponse response;
        response.mimeType = mimeForPath(path);
        response.data.assign(bytes->begin(), bytes->end());
        return response;
    };
}

WebView::Options makeOptionsFromEmbedded(const WebView::EmbeddedOptions& embedded)
{
    WebView::Options options;
    options.schemes[embedded.scheme] =
        makeResourceProviderFromFiles(embedded.provider, embedded.indexFile);
    return options;
}
} // namespace

WebView::WebView()
    : WebView(Options {})
{
}

WebView::WebView(Options options)
    : impl(*this, std::move(options))
{
    impl->attachToParentView();
}

WebView::WebView(EmbeddedOptions options)
    : WebView(options.devServerURL.empty() ? makeOptionsFromEmbedded(options)
                                           : Options {})
{
    if (! options.autoLoad)
        return;

    if (! options.devServerURL.empty())
        loadURL(options.devServerURL);
    else
        loadURL(options.scheme + "://" + options.host + "/" + options.indexFile);
}

WebView::~WebView()
{
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

void WebView::resized()
{
    View::resized();
    impl->updateFrame();
}
} // namespace eacp::Graphics
