#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

#include "WebViewPlatform-Apple.h"

#include <eacp/Core/ObjC/Strings.h>

namespace eacp::Graphics::detail
{
void attachWKWebViewToParent(WKWebView* webView, void* parentHandle)
{
    auto* parentView = (__bridge UIView*) parentHandle;
    [parentView addSubview:webView];
}

void applyNativeZoom(WKWebView* webView, double clamped, double& storedZoom)
{
    storedZoom = clamped;
    auto script = std::string("document.documentElement.style.zoom = '")
        + std::to_string(clamped) + "';";
    [webView evaluateJavaScript:Strings::toNSString(script)
              completionHandler:nil];
}

double readNativeZoom(WKWebView*, double storedZoom)
{
    return storedZoom;
}

WebView* findFocusedWebView()
{
    auto& registered = registeredWebViews();
    return registered.empty() ? nullptr : registered.back();
}

WKWebView* createWebView(WKWebViewConfiguration* config, const WebKitOptions&)
{
    auto rect = CGRectMake(0, 0, 100, 100);
    return [[WKWebView alloc] initWithFrame:rect configuration:config];
}

void armFileDrag(WKWebView*, const Vector<std::string>&)
{
    assert(false && "armFileDrag is macOS-only");
}

void setFileDragStartedCallback(WKWebView*, Callback)
{
}

void armWindowDrag(WKWebView*)
{
    assert(false && "armWindowDrag is macOS-only");
}

void performWindowControl(WKWebView*, const std::string&)
{
    assert(false && "performWindowControl is macOS-only");
}

void setUnhandledKeyCallback(WKWebView*, UnhandledNSKeyCallback)
{
    // Key forwarding is macOS-only.
}

void reportKeyVerdict(WKWebView*, bool, bool)
{
}
} // namespace eacp::Graphics::detail
