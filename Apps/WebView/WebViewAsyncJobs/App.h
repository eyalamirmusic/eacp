#pragma once

#include "Types.h"
#include <eacp/WebView/WebView.h>

using namespace eacp;

struct MyApp
{
    MyApp()
    {
        Graphics::setApplicationMenuBar(Graphics::buildDefaultWebViewMenuBar());
        window.setContentView(webView);
    }

    Api::SearchApi search;
    Graphics::WebView webView {Graphics::embeddedOptions("AsyncJobsApp")};
    Graphics::WebViewBridge transport {webView, search};
    Graphics::Window window;
};
