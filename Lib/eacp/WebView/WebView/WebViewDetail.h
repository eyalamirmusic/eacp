#pragma once

#include "WebView.h"

namespace eacp::Graphics::detail
{
// Process-wide registry of live WebViews, populated by every backend's
// constructor / destructor and defined once in WebView-Shared.cpp. Resolving
// the *focused* WebView stays platform-specific (findFocusedWebView on Apple,
// WebView::focused on Windows), but the registry itself is shared. Main-thread
// only, so no locking is needed.
Vector<WebView*>& registeredWebViews();
void registerWebView(WebView* view);
void unregisterWebView(WebView* view);

// Clamps a requested zoom factor to the supported range. Shared so both
// backends' setZoom apply the same limits as the shared zoomIn/zoomOut steps.
double clampZoom(double level);
} // namespace eacp::Graphics::detail
