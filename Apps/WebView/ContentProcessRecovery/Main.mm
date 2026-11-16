#include <eacp/WebView/WebView.h>

#import <WebKit/WebKit.h>

#include <csignal>

// macOS kills a WebView's content process under memory pressure and across
// sleep. Press the button on the page to kill it the same way (SIGKILL): the
// page comes back on its own, with a new load time, and the count of restarts
// goes up by one.

// Private, as Safari's own tests use it: the pid of the view's content process.
@interface WKWebView (ContentProcessRecoveryDemo)
- (pid_t)_webProcessIdentifier;
@end

using namespace eacp;

namespace
{
const std::string pageHtml = R"HTML(<!doctype html>
<html><body style="font: 16px -apple-system; padding: 24px">
<h2>Content process recovery</h2>
<p>This page loaded at <b id="loadTime"></b>.</p>
<p>Content process restarts: <b id="restarts">0</b></p>
<button onclick="window.webkit.messageHandlers.killContentProcess.postMessage('')">
Kill the content process (SIGKILL)</button>
<script>document.getElementById('loadTime').textContent = new Date().toLocaleTimeString();</script>
</body></html>)HTML";

Graphics::WebView::Options makeOptions()
{
    auto options = Graphics::WebView::Options {};
    options.schemes["recovery"] =
        [](std::string_view) -> std::optional<Graphics::ResourceResponse>
    {
        auto response = Graphics::ResourceResponse {};
        response.mimeType = "text/html; charset=utf-8";

        for (auto character: pageHtml)
            response.data.add(static_cast<std::uint8_t>(character));

        return response;
    };

    return options;
}

WKWebView* findWKWebView(NSView* view)
{
    if ([view isKindOfClass:[WKWebView class]])
        return (WKWebView*) view;

    for (NSView* child in view.subviews)
    {
        if (auto* found = findWKWebView(child))
            return found;
    }

    return nil;
}

struct RecoveryDemo final : Graphics::View
{
    RecoveryDemo()
    {
        webView.addScriptMessageHandler("killContentProcess",
                                        [this](const std::string&)
                                        { killContentProcess(); });

        webView.onContentProcessTerminated = [this] { ++restarts; };
        webView.onNavigationFinished = [this](const std::string&) { showRestarts(); };
        webView.loadURL("recovery://demo/index.html");

        addChildren({webView});
    }

    void resized() override { scaleToFit({webView}); }

    void killContentProcess()
    {
        auto* wkWebView = findWKWebView((__bridge NSView*) webView.getHandle());

        if (wkWebView != nil)
            kill([wkWebView _webProcessIdentifier], SIGKILL);
    }

    void showRestarts()
    {
        webView.evaluateJavaScript("document.getElementById('restarts').textContent = "
                                   + std::to_string(restarts));
    }

    Graphics::WebView webView {makeOptions()};
    int restarts = 0;
};
} // namespace

int main()
{
    auto options = Graphics::WindowOptions {};
    options.title = "eacp — content process recovery";
    return Graphics::runWindowedApp<RecoveryDemo>(options);
}
