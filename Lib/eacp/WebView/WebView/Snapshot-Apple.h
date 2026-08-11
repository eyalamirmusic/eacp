#pragma once

#include "WebView.h"

@class WKWebView;

namespace eacp::Graphics::detail
{
// Delivers PNG bytes, or an error message, to `callback`.
void takeAppleSnapshot(WKWebView* webView, WebView::SnapshotCallback callback);
} // namespace eacp::Graphics::detail
