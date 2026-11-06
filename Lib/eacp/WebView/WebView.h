#pragma once

/** @file
    A native web view (WKWebView on Apple, WebView2 on Windows) and the bridge
    between its page and C++: typed commands, events and synced state over
    Miro, with TypeScript generated from the C++ schema at build time.
    The WebView itself needs `EACP_HAS_WEBVIEW` (Apple and Windows); the page
    bridge over a `ScriptHost` (`eacp-webview-bridge`) builds everywhere.

    Entry points: `Graphics::WebView` for the view, `Bridge` for commands and
    events, `EACP_STATE` for state the page mirrors. `EACP_WEBVIEW_DEV=ON`
    serves the UI from the Vite dev server instead of embedded resources.
    `WebView::Test` (`Test/`) drives a live page from tests.
*/

#include "WebView/Bridge.h"
#include "WebView/MenuHelpers.h"
#include "WebView/ScriptHost.h"
#include "WebView/StateBridge.h"
#include "WebView/WebView.h"
