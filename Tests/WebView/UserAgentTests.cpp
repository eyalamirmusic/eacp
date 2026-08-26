#include "Common.h"
// Options::userAgent and Options::loadDeclinedPopupsInline on a live WebView.
// The user agent is read back from the page itself rather than from the
// option, and the popup flag is checked where it actually shows: a declined
// window.open either replaces the page under the app or leaves it standing.
// Both drive about:blank, so neither test touches the network.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
constexpr auto customUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) eacpTests/1.0 Safari/605.1.15";

// Reports its user agent, and keeps a marker that only survives while this
// document does — which is how the popup tests tell a navigation from a
// no-op.
const std::string reportingPage = R"HTML(<!doctype html><html><body><script>
  window.marker = 'alive';
  window.openPopup = function () { window.open('about:blank', '_blank'); };
  window.webkit.messageHandlers.ua.postMessage(navigator.userAgent);
</script></body></html>)HTML";

struct Fixture
{
    explicit Fixture(WebView::Options options)
        : webView(std::move(options))
    {
        window.setContentView(webView);

        webView.addScriptMessageHandler("ua",
                                        [this](const std::string& message)
                                        {
                                            reportedUserAgent = message;
                                            loaded = true;
                                        });

        webView.onNewWindowRequested = [this](OwningPointer<WebView>, auto&&)
        {
            declined = true;
            return false;
        };

        // The page's own load finishes *after* its script has posted the
        // user agent, so the watch only opens once both have happened —
        // otherwise the load being tested for is already recorded before the
        // test has done anything.
        webView.onNavigationFinished = [this](auto&&)
        {
            if (watching)
                navigatedWhileWatching = true;
            else
                initialNavigationDone = true;
        };

        webView.loadHTML(reportingPage);
        check(Threads::runEventLoopUntil([this]
                                         { return loaded && initialNavigationDone; },
                                         firstNavigationTimeout));

        watching = true;
    }

    std::string readMarker()
    {
        return webView.callJS("String(window.marker)").waitFor(webViewResultTimeout);
    }

    WebView webView;
    Window window {};
    std::string reportedUserAgent;
    bool loaded = false;
    bool declined = false;
    bool initialNavigationDone = false;
    bool watching = false;
    bool navigatedWhileWatching = false;
};

WebView::Options withUserAgent(const std::string& userAgent)
{
    auto options = WebView::Options {};
    options.userAgent = userAgent;
    return options;
}

WebView::Options withInlineLoading(bool inlineLoading)
{
    auto options = WebView::Options {};
    options.loadDeclinedPopupsInline = inlineLoading;
    return options;
}
} // namespace

auto tUserAgentDefaultsEmpty = test("WebViewOptions/userAgentDefaultsEmpty") = []
{ check(WebView::Options {}.userAgent.empty()); };

auto tInlineLoadingDefaultsOn =
    test("WebViewOptions/loadDeclinedPopupsInlineDefaultsOn") = []
{ check(WebView::Options {}.loadDeclinedPopupsInline); };

auto tDefaultUserAgentIsThePlatformOne =
    test("WebView/emptyUserAgentLeavesThePlatformOne") = []
{
    auto fix = Fixture {WebView::Options {}};

    check(!fix.reportedUserAgent.empty());
    check(fix.reportedUserAgent != customUserAgent);
};

auto tUserAgentOverrideReachesThePage =
    test("WebView/userAgentOverrideReachesThePage") = []
{
    auto fix = Fixture {withUserAgent(customUserAgent)};

    check(fix.reportedUserAgent == customUserAgent);
};

// The behaviour every embedder has had: nowhere else to put the navigation,
// so it lands here and the document is replaced.
auto tDeclinedPopupLoadsInlineByDefault =
    test("WebView/declinedPopupLoadsInlineByDefault") = []
{
    auto fix = Fixture {withInlineLoading(true)};

    check(fix.readMarker() == "alive");

    fix.webView.evaluateJavaScript("window.openPopup()");

    check(Threads::runEventLoopUntil([&] { return fix.declined; },
                                     webViewResultTimeout));
    check(Threads::runEventLoopUntil([&] { return fix.navigatedWhileWatching; },
                                     webViewResultTimeout));
    check(fix.readMarker() == "undefined");
};

// And the behaviour a container needs: the app routed the URL somewhere else,
// so the page it declined for must still be there afterwards.
auto tDeclinedPopupCanLeaveTheHostAlone =
    test("WebView/declinedPopupCanLeaveTheHostAlone") = []
{
    auto fix = Fixture {withInlineLoading(false)};

    fix.webView.evaluateJavaScript("window.openPopup()");

    check(Threads::runEventLoopUntil([&] { return fix.declined; },
                                     webViewResultTimeout));

    // The inline load, had it happened, would be in flight by now.
    Threads::runEventLoopFor(Time::MS {500});

    check(!fix.navigatedWhileWatching);
    check(fix.readMarker() == "alive");
};
