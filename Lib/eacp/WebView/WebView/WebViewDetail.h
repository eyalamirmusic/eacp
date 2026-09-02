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

// Long enough that no healthy snapshot ever reaches it — a warm one costs
// single-digit milliseconds, and even a hidden page on a saturated CI runner
// stays inside a couple of hundred — and short enough to land well inside the
// timeouts callers actually wait with, so a wedged snapshot surfaces as an
// empty image rather than as a race against the caller's own deadline.
constexpr auto snapshotDeadline = Time::MS {5000};

// Wraps a snapshot callback so it runs exactly once and always runs: with the
// platform's answer if that arrives, with an error at the deadline if it does
// not. Both native snapshot calls can go quiet — WKWebView drops the completion
// handler of a snapshot taken against a hidden, throttled web process, and
// WebView2's CapturePreview never completes if the browser process dies mid
// capture — and everything downstream settles only when the callback runs, so
// an unanswered snapshot used to hang the caller rather than fail it. Applied
// by each backend's takeSnapshot, so the guarantee is stated once. The deadline
// is a parameter only so a test can assert the behaviour without waiting out
// the real one.
WebView::SnapshotCallback withSnapshotDeadline(WebView::SnapshotCallback callback,
                                               Time::MS deadline = snapshotDeadline);
} // namespace eacp::Graphics::detail
