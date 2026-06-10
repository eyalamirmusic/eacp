#pragma once

#include "WebView.h"
#include "WebViewDetail.h"

@class WKWebView;
@class WKWebViewConfiguration;

namespace eacp::Graphics::detail
{
// wkWebViewOf is implemented in WebView.mm so the platform translation units can
// reach the WKWebView held inside Native without pulling Native's full
// definition into a header. The shared WebView registry lives in
// WebViewDetail.h / WebView-Shared.cpp.
WKWebView* wkWebViewOf(WebView* view);

// Platform-specific implementations live in WebViewPlatform-iOS.mm and
// WebViewPlatform-macOS.mm — the CMake IOS branch selects the right TU.
void attachWKWebViewToParent(WKWebView* webView, void* parentHandle);
void applyNativeZoom(WKWebView* webView, double clamped, double& storedZoom);
double readNativeZoom(WKWebView* webView, double storedZoom);
WebView* findFocusedWebView();

// Per-view WebKit knobs applied at creation. Grow this struct, not the
// createWebView signature, as more platform behaviours become configurable.
struct WebKitOptions
{
    // macOS: deliver the first click on an unfocused window to the page
    // (NSView acceptsFirstMouse) so drag regions work without a focusing
    // click first. Ignored on iOS.
    bool acceptFirstMouse = false;
};

// Creates the platform WKWebView. On macOS this is a subclass that owns native
// file drag-out (intercepts mouseDragged: to start the session from a live
// event); on iOS it is a plain WKWebView.
WKWebView* createWebView(WKWebViewConfiguration* config,
                         const WebKitOptions& options);

// Arms a native file drag-out for the next mouse gesture with the given on-disk
// paths. macOS-only behaviour; the iOS translation unit provides a no-op.
void armFileDrag(WKWebView* webView, const Vector<std::string>& paths);

// Arms a native window drag for the next mouse gesture. macOS-only; the iOS
// translation unit provides a no-op.
void armWindowDrag(WKWebView* webView);

// Applies a caption-button action to the window hosting the web view, posted
// by the injected window-controls.js and parsed in WebView-Shared.cpp.
// macOS-only; the iOS translation unit provides no-ops.
void applyWindowControl(WKWebView* webView, WindowControlAction action);
bool isWindowMaximized(WKWebView* webView);
} // namespace eacp::Graphics::detail
