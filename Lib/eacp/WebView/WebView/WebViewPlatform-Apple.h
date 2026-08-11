#pragma once

#include "WebView.h"
#include "WebViewDetail.h"

@class WKWebView;
@class WKWebViewConfiguration;
@class NSEvent;

namespace eacp::Graphics::detail
{
// Implemented in WebView.mm, so platform TUs can reach the WKWebView without
// Native's full definition entering a header.
WKWebView* wkWebViewOf(WebView* view);

void attachWKWebViewToParent(WKWebView* webView, void* parentHandle);
void applyNativeZoom(WKWebView* webView, double clamped, double& storedZoom);
double readNativeZoom(WKWebView* webView, double storedZoom);
WebView* findFocusedWebView();

// Per-view WebKit knobs applied at creation.
struct WebKitOptions
{
    // macOS NSView acceptsFirstMouse; ignored on iOS.
    bool acceptFirstMouse = false;
};

// On macOS, a subclass that intercepts mouseDragged: to start a file drag.
WKWebView* createWebView(WKWebViewConfiguration* config,
                         const WebKitOptions& options);

// Arms the next mouse gesture; macOS-only, a no-op on iOS.
void armFileDrag(WKWebView* webView, const Vector<std::string>& paths);
void setFileDragStartedCallback(WKWebView* webView, Callback callback);
void armWindowDrag(WKWebView* webView);

// "minimize" / "maximize" / "close", posted by the injected
// window-controls.js. macOS-only.
void performWindowControl(WKWebView* webView, const std::string& action);

// Pairs with the injected key-events.js: the view stashes each incoming key
// NSEvent, and reportKeyVerdict consumes the page's verdicts in delivery
// order, firing the callback for events the page left unhandled. macOS-only.
using UnhandledNSKeyCallback = std::function<void(NSEvent* event, bool isDown)>;
void setUnhandledKeyCallback(WKWebView* webView, UnhandledNSKeyCallback callback);
void reportKeyVerdict(WKWebView* webView, bool isDown, bool consumed);
} // namespace eacp::Graphics::detail
