#include "Common.h"
// macOS jetsams WebContent processes routinely (memory pressure, sleep/wake).
// A WKWebView whose content process died renders an opaque dead view until
// someone reloads it — and WebKit only tells the navigation delegate, via
// webViewWebContentProcessDidTerminate:. A delegate that does not implement
// the selector leaves the app permanently black until relaunch.
//
// This suite kills the live WebContent process for real (SIGKILL, the same
// thing the jetsam does) and requires the page to come back on its own. The
// page is served the way embedding apps serve theirs — a custom scheme with a
// resource provider — and loaded with loadURL.

#import <WebKit/WebKit.h>

#include <csignal>

using namespace nano;
using namespace eacp;

// Test-only SPI: the pid of the view's WebContent process, so the test can
// kill it exactly like the OS does.
@interface WKWebView (EacpTerminationTesting)
- (pid_t)_webProcessIdentifier;
@end

namespace
{
// Counts its own loads: `ready` fires every time the script runs, so a reload
// after the kill is visible as a second message.
const std::string pageHtml = R"HTML(<!doctype html><html><body><script>
  window.webkit.messageHandlers.ready.postMessage('ready');
</script></body></html>)HTML";

Graphics::WebView::Options servedOptions()
{
    auto options = Graphics::WebView::Options {};
    options.schemes["testterm"] =
        [](std::string_view) -> std::optional<Graphics::ResourceResponse>
    {
        auto response = Graphics::ResourceResponse {};
        response.mimeType = "text/html; charset=utf-8";
        for (auto character: pageHtml)
            response.data.add(static_cast<std::uint8_t>(character));
        return response;
    };
    options.userDataFolderSuffix = "contentprocesstermination";
    return options;
}

WKWebView* findWKWebView(NSView* view)
{
    if ([view isKindOfClass:[WKWebView class]])
        return (WKWebView*) view;

    for (NSView* child in view.subviews)
        if (auto* found = findWKWebView(child))
            return found;

    return nil;
}

enum class LoadStyle
{
    schemeURL,
    htmlString
};

struct Fixture
{
    // Qualified: the legacy WebKit header also declares an ObjC `WebView`.
    Graphics::WebView webView {servedOptions()};
    Graphics::Window window {};
    int loads = 0;
    int terminations = 0;

    explicit Fixture(LoadStyle style = LoadStyle::schemeURL)
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler("ready",
                                        [this](const std::string&) { ++loads; });
        webView.onContentProcessTerminated = [this] { ++terminations; };

        if (style == LoadStyle::schemeURL)
            webView.loadURL("testterm://host/index.html");
        else
            webView.loadHTML(pageHtml);

        check(Threads::runEventLoopUntil([this] { return loads == 1; },
                                         firstNavigationTimeout));
    }

    WKWebView* wkWebView()
    {
        auto* nsWindow = (NSWindow*) window.getHandle();
        auto* found = findWKWebView(nsWindow.contentView);
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

// The contract with WebKit: it checks respondsToSelector: before telling
// anyone the process died. No implementation, no notification, dead view.
auto tDelegateHearsAboutTermination =
    test("ContentProcessTermination/delegateImplementsTheSelector") = []
{
    auto fix = Fixture {};

    check([(NSObject*) fix.wkWebView().navigationDelegate
        respondsToSelector:@selector(webViewWebContentProcessDidTerminate:)]);
};

auto tPageComesBackAfterProcessDeath =
    test("ContentProcessTermination/pageReloadsAfterContentProcessDies") = []
{
    auto fix = Fixture {};

    fix.killContentProcess();

    // The page script runs again only if something reloaded the dead view.
    check(Threads::runEventLoopUntil([&fix] { return fix.loads >= 2; },
                                     firstNavigationTimeout));
    check(fix.terminations == 1);
};

// The other load shape: an HTML-string load leaves no back-forward item to
// reload (and WebKit will not honour a re-issued loadHTMLString against the
// replacement process — it dedupes to an empty about:blank). There the owner
// callback IS the recovery path, so an embedder that loadHTMLs can re-issue
// its own load — which is exactly what this does, and the page comes back.
auto tHtmlStringEmbedderRecoversViaCallback =
    test("ContentProcessTermination/htmlStringEmbedderRecoversViaCallback") = []
{
    auto fix = Fixture {LoadStyle::htmlString};
    fix.webView.onContentProcessTerminated = [&fix]
    {
        ++fix.terminations;
        fix.webView.loadHTML(pageHtml);
    };

    fix.killContentProcess();

    check(Threads::runEventLoopUntil([&fix] { return fix.loads >= 2; },
                                     firstNavigationTimeout));
    check(fix.terminations == 1);
};
