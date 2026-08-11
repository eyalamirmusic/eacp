#include "Common.h"

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

// Off matches WKWebView, which has no status bar at all; Windows opts in.
auto tStatusBarDefaultsOff = test("WebViewOptions/statusBarDefaultsOff") = []
{
    auto options = WebView::Options {};
    check(!options.statusBar);
};

auto tStatusBarIsOptIn = test("WebViewOptions/statusBarIsOptIn") = []
{
    auto options = WebView::Options {};
    options.statusBar = true;
    check(options.statusBar);
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

// -[WKUserContentController addScriptMessageHandler:name:] RAISES on a name it
// already holds, and the controller outlives every page the WebView loads — so
// a caller that attaches a channel per editor-open sees the raise on the second
// open. Uncaught in an AU view service, that is a dead host process.
auto tDuplicateScriptMessageHandlerNameIsSafe =
    test("WebView/duplicateScriptMessageHandlerNameIsSafe") = []
{
    auto webView = WebView {};

    webView.addScriptMessageHandler("host", [](const std::string&) {});
    webView.addScriptMessageHandler("host", [](const std::string&) {});

    check(true);
};

auto tScriptMessageHandlerReattachesAfterRemove =
    test("WebView/scriptMessageHandlerReattachesAfterRemove") = []
{
    auto webView = WebView {};

    webView.addScriptMessageHandler("host", [](const std::string&) {});
    webView.removeScriptMessageHandler("host");
    webView.addScriptMessageHandler("host", [](const std::string&) {});

    check(true);
};
