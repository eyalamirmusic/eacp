#pragma once

#include "Types.h"
#include <eacp/WebView/WebView.h>

using namespace eacp;

inline Graphics::WindowOptions calculatorWindowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.title = "Calculator";
    options.width = 340;
    options.height = 540;
    options.minWidth = 280;
    options.minHeight = 440;
    return options;
}

struct MyApp
{
    MyApp()
    {
        Graphics::setApplicationMenuBar(Graphics::buildDefaultWebViewMenuBar());
        window.setContentView(webView);
    }

    Api::CalculatorApi calculatorApi;
    Graphics::WebView webView {Graphics::embeddedOptions("CalculatorApp")};
    Graphics::WebViewBridge transport {webView, calculatorApi};
    Graphics::Window window {calculatorWindowOptions()};
};
