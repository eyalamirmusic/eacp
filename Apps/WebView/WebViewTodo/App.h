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

    Api::TodosApi todos;
    Graphics::WebView webView {Graphics::embeddedOptions("TodoApp")};
    Graphics::WebViewBridge transport {webView, todos};
    Graphics::Window window;
};
