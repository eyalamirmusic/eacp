#include <eacp/WebView/WebView.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

auto tTransparentBackgroundDefaultsOff =
    test("WebViewOptions/transparentBackgroundDefaultsOff") = []
{
    auto options = WebView::Options {};
    check(!options.transparentBackground);
};

auto tTransparentBackgroundOptionIsOptIn =
    test("WebViewOptions/transparentBackgroundIsOptIn") = []
{
    auto options = WebView::Options {};
    options.transparentBackground = true;
    check(options.transparentBackground);
};

auto tFileDragStartedCallbackIsUserOwned =
    test("WebView/fileDragStartedCallbackIsUserOwned") = []
{
    auto webView = WebView {};
    auto calls = 0;

    webView.onFileDragStarted = [&] { ++calls; };
    webView.onFileDragStarted();
    webView.onFileDragStarted();

    check(calls == 2);
};

auto tFocusContentIsSafeBeforeNavigation =
    test("WebView/focusContentIsSafeBeforeNavigation") = []
{
    auto webView = WebView {};
    auto window = Window {};
    window.setContentView(webView);

    webView.focusContent();
    check(true);
};
