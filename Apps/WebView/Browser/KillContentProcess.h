#pragma once

#include <eacp/WebView/WebView.h>

// Kills the process that runs the view's page: WebKit's WebContent process on
// Apple platforms, the WebView2 renderer on Windows.
void killContentProcess(eacp::Graphics::WebView& webView);
