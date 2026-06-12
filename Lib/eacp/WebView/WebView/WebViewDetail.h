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

// The window-control wire protocol shared with window-controls.js: the shim
// posts one of these action names over the __eacpWindowControl message
// handler (the JS side keeps its own copies; WindowControlTests pin the two
// in sync). Parsing happens once here — platform code only ever sees the
// enum.
inline constexpr std::string_view windowControlMinimize = "minimize";
inline constexpr std::string_view windowControlMaximize = "maximize";
inline constexpr std::string_view windowControlClose = "close";

std::optional<WindowControlAction>
    parseWindowControlAction(const std::string& action);

// Script that mirrors the post-toggle maximize state onto the page's
// data-eacp-maximized attribute (window-controls.js defines the setter).
std::string getSetMaximizedScript(bool maximized);
} // namespace eacp::Graphics::detail
