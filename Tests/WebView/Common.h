#pragma once

#include <eacp/WebView/WebView.h>
#include <NanoTest/NanoTest.h>
#include <thread>

// Booting the engine and running document-start injection is by far the
// slowest step in these suites, and a contended CI runner has overshot 10s.
//
// The budget also has to cover a failed first launch *and* its retry, not just
// a slow one. When the WebView2 browser process dies during creation on a CI
// runner, the abort surfaces ~36s in, and the backend answers it by relaunching
// against a fresh environment (shouldRetryWebView2Create in WebView-Windows.cpp).
// At 30s the wait expired before that relaunch could finish, so the retry could
// never actually rescue the test. A healthy first navigation still costs ~8s,
// and the wait returns as soon as it settles, so the larger ceiling is only
// ever spent on a launch that already went wrong.
constexpr auto firstNavigationTimeout = eacp::Time::MS {90000};

// A round trip through an already-live page.
constexpr auto webViewResultTimeout = eacp::Time::MS {10000};
