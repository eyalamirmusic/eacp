#pragma once

#include "WebView.h"

namespace eacp::Graphics::detail
{
// Process-wide registry of live WebViews. Main-thread only, so unlocked.
Vector<WebView*>& registeredWebViews();
void registerWebView(WebView* view);
void unregisterWebView(WebView* view);

double clampZoom(double level);
} // namespace eacp::Graphics::detail
