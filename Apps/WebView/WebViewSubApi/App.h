#pragma once

#include "Types.h"
#include <eacp/WebView/WebView.h>

using namespace eacp;

struct MyApp
{
    MyApp()
    {
        Graphics::setApplicationMenuBar(Graphics::buildDefaultWebViewMenuBar(), window);
        window.setContentView(webView);
    }

    Api::RootApi root;
    Graphics::WebView webView {Graphics::embeddedOptions("SubApiApp")};
    Graphics::WebViewBridge transport {webView, root};
    Graphics::Window window;
};
