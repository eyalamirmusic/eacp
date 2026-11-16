#include "Common.h"
// SIGKILLs the view's WebContent process, as macOS does; the page must come back.

#import <WebKit/WKNavigationDelegate.h>
#import <WebKit/WKWebView.h>

#include <eacp/WebView/WebView/WebViewPlatform-Apple.h>

#include <csignal>

using namespace nano;
using namespace eacp;

// Private API: the pid of the view's WebContent process.
@interface WKWebView (EacpTerminationTesting)
- (pid_t)_webProcessIdentifier;
@end

namespace
{
// Posts `ready` on every load, so a reload after the kill is a second message.
const std::string contentProcessPageHtml = R"HTML(<!doctype html><html><body><script>
  window.webkit.messageHandlers.ready.postMessage('ready');
</script></body></html>)HTML";

Graphics::WebView::Options makeContentProcessOptions()
{
    auto options = Graphics::WebView::Options {};
    options.schemes["testterm"] =
        [](std::string_view) -> std::optional<Graphics::ResourceResponse>
    {
        auto response = Graphics::ResourceResponse {};
        response.mimeType = "text/html; charset=utf-8";
        response.data.assign(contentProcessPageHtml.begin(),
                             contentProcessPageHtml.end());
        return response;
    };
    options.userDataFolderSuffix = "contentprocesstermination";
    return options;
}

enum class ContentProcessLoadStyle
{
    schemeURL,
    htmlString
};

struct ContentProcessFixture
{
    Graphics::WebView webView {makeContentProcessOptions()};
    Graphics::Window window {};
    int loads = 0;
    int terminations = 0;

    explicit ContentProcessFixture(ContentProcessLoadStyle style = ContentProcessLoadStyle::schemeURL)
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler("ready",
                                        [this](const std::string&) { ++loads; });
        webView.onContentProcessTerminated = [this] { ++terminations; };

        if (style == ContentProcessLoadStyle::schemeURL)
            webView.loadURL("testterm://host/index.html");
        else
            webView.loadHTML(contentProcessPageHtml);

        check(Threads::runEventLoopUntil([this] { return loads == 1; },
                                         firstNavigationTimeout));
    }

    WKWebView* wkWebView()
    {
        auto* found = Graphics::detail::wkWebViewOf(&webView);
        check(found != nil);
        return found;
    }

    void killContentProcess()
    {
        auto pid = [wkWebView() _webProcessIdentifier];
        check(pid > 0);
        kill(pid, SIGKILL);
    }
};
} // namespace

// WebKit reports the death only to a delegate that responds to this selector.
auto tDelegateHearsAboutTermination =
    test("ContentProcessTermination/delegateImplementsTheSelector") = []
{
    auto fix = ContentProcessFixture {};

    auto* delegate = (NSObject*) fix.wkWebView().navigationDelegate;
    check([delegate respondsToSelector:@selector(webViewWebContentProcessDidTerminate:)]);
};

auto tPageComesBackAfterProcessDeath =
    test("ContentProcessTermination/pageReloadsAfterContentProcessDies") = []
{
    auto fix = ContentProcessFixture {};

    fix.killContentProcess();

    // The page script runs again only if something reloaded the dead view.
    check(Threads::runEventLoopUntil([&fix] { return fix.loads >= 2; },
                                     firstNavigationTimeout));
    check(fix.terminations == 1);
};

// A loadHTML page has nothing to reload: the embedder re-issues it from the callback.
auto tHtmlStringEmbedderRecoversViaCallback =
    test("ContentProcessTermination/htmlStringEmbedderRecoversViaCallback") = []
{
    auto fix = ContentProcessFixture {ContentProcessLoadStyle::htmlString};
    fix.webView.onContentProcessTerminated = [&fix]
    {
        ++fix.terminations;
        fix.webView.loadHTML(contentProcessPageHtml);
    };

    fix.killContentProcess();

    check(Threads::runEventLoopUntil([&fix] { return fix.loads >= 2; },
                                     firstNavigationTimeout));
    check(fix.terminations == 1);
};
