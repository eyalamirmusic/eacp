#pragma once

#include "Types.h"
#include <eacp/WebView/WebView.h>

using namespace eacp;

inline Graphics::WindowOptions todoWindowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.title = "Todos";
    options.width = 560;
    options.height = 760;
    return options;
}

struct MyApp
{
    MyApp()
    {
        Graphics::setApplicationMenuBar(Graphics::buildDefaultWebViewMenuBar());
        window.setContentView(webView);
    }

    Api::TodosApi todos;
    Graphics::WebView webView {Graphics::embeddedOptions("TodoApp")};
    Graphics::WebViewBridge transport {webView, todos};
    Graphics::Window window {todoWindowOptions()};
};
