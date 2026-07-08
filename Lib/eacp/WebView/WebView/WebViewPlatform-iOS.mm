#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

#include "WebViewPlatform-Apple.h"

#include <eacp/Core/ObjC/Strings.h>

#include <cassert>
#include <string>

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
    // Native file drag-out is a macOS desktop affordance, not implemented here.
    assert(false && "armFileDrag is macOS-only");
}

void setFileDragStartedCallback(WKWebView*, Callback)
{
}

void armWindowDrag(WKWebView*)
{
    // Window dragging is a desktop affordance; iOS windows aren't movable.
    assert(false && "armWindowDrag is macOS-only");
}

void performWindowControl(WKWebView*, const std::string&)
{
    // Caption buttons are a desktop affordance; the shim that posts these
    // actions is only installed on macOS.
    assert(false && "performWindowControl is macOS-only");
}

void setUnhandledKeyCallback(WKWebView*, UnhandledNSKeyCallback)
{
    // Key forwarding targets desktop responder chains; the shim that produces
    // verdicts is only installed on macOS.
}

void reportKeyVerdict(WKWebView*, bool, bool)
{
}
} // namespace eacp::Graphics::detail
