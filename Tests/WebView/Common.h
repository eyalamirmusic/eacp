#pragma once

#include <eacp/WebView/WebView.h>
#include <NanoTest/NanoTest.h>
#include <thread>

// Booting the engine and running document-start injection is by far the
// slowest step in these suites, and a contended CI runner has overshot 10s.
constexpr auto firstNavigationTimeout = eacp::Time::MS {30000};

constexpr auto webViewResultTimeout = eacp::Time::MS {10000};
