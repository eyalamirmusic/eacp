#pragma once

/** @file Internal: PNG snapshots of a WKWebView on macOS and iOS. */

#include "WebView.h"

@class WKWebView;

namespace eacp::Graphics::detail
{
// Captures a PNG snapshot of `webView` and delivers the bytes (or an
// error message) via `callback`. The macOS and iOS implementations
// live in Snapshot-macOS.mm and Snapshot-iOS.mm respectively — the
// build system selects the right TU.
void takeAppleSnapshot(WKWebView* webView, WebView::SnapshotCallback callback);
} // namespace eacp::Graphics::detail
