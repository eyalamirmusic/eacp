#pragma once

#include "WebView.h"

@class WKWebView;
@class WKWebViewConfiguration;

namespace eacp::Graphics::detail
{
// Shared accessors implemented in WebView.mm so the platform translation
// units can reach the registry and the WKWebView held inside Native
// without pulling Native's full definition into a header.
EA::Vector<WebView*>& registeredWebViews();
WKWebView* wkWebViewOf(WebView* view);

// Platform-specific implementations live in WebViewPlatform-iOS.mm and
// WebViewPlatform-macOS.mm — the CMake IOS branch selects the right TU.
void attachWKWebViewToParent(WKWebView* webView, void* parentHandle);
void applyNativeZoom(WKWebView* webView, double clamped, double& storedZoom);
double readNativeZoom(WKWebView* webView, double storedZoom);
WebView* findFocusedWebView();

// Creates the platform WKWebView. On macOS this is a subclass that owns native
// file drag-out (intercepts mouseDragged: to start the session from a live
// event); on iOS it is a plain WKWebView.
WKWebView* createWebView(WKWebViewConfiguration* config);

// Arms a native file drag-out for the next mouse gesture with the given on-disk
// paths. macOS-only behaviour; the iOS translation unit provides a no-op.
void armFileDrag(WKWebView* webView, const std::vector<std::string>& paths);

// Arms a native window drag for the next mouse gesture. macOS-only; the iOS
// translation unit provides a no-op.
void armWindowDrag(WKWebView* webView);
} // namespace eacp::Graphics::detail
