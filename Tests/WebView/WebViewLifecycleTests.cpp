#include "Common.h"

// A real WebView created and destroyed over and over, sized and resized inside
// a parent view, and handed page messages of every body type.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace WebViewLifecycleTests
{
constexpr auto repeatedOpenCount = 4;
const auto firstBounds = Rect {0, 0, 320, 240};
const auto secondBounds = Rect {10, 20, 410, 300};
const auto windowSize = Point {640, 480};
constexpr auto settleAfterTeardown = Time::MS {200};

const std::string lifecyclePageHtml = R"HTML(<!doctype html><html>
<body style="margin:0"><script>
window.reportSize = () => window.innerWidth + 'x' + window.innerHeight;
window.webkit.messageHandlers.ready.postMessage('ready');
</script></body></html>)HTML";

struct LifecycleFixture
{
    View container;
    WebView webView {};
    Window window {};
    bool ready = false;
    Vector<std::string> received;

    LifecycleFixture()
    {
        window.setSize(windowSize);
        container.addSubview(webView);
        webView.setBounds(firstBounds);
        window.setContentView(container);

        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.addScriptMessageHandler(
            "probe", [this](const std::string& body) { received.add(body); });
        webView.loadHTML(lifecyclePageHtml);
    }

    bool waitUntilReady()
    {
        return Threads::runEventLoopUntil([this] { return ready; },
                                          firstNavigationTimeout);
    }

    std::string runScript(const std::string& script)
    {
        return webView.callJS(script).waitFor(webViewResultTimeout);
    }

    bool waitForPageSize(const Rect& bounds)
    {
        auto expected = std::to_string(static_cast<int>(bounds.w)) + "x"
                        + std::to_string(static_cast<int>(bounds.h));

        return Threads::runEventLoopUntil(
            [this, expected]
            { return runScript("window.reportSize()") == expected; },
            webViewResultTimeout);
    }

    std::string postAndReceive(const std::string& bodyExpression)
    {
        auto expectedCount = received.size() + 1;
        runScript("window.webkit.messageHandlers.probe.postMessage(" + bodyExpression
                  + "); 'ok'");

        auto arrived = Threads::runEventLoopUntil(
            [this, expectedCount] { return received.size() >= expectedCount; },
            webViewResultTimeout);
        check(arrived, bodyExpression);
        return arrived ? received.back() : std::string {};
    }
};
} // namespace WebViewLifecycleTests

using namespace WebViewLifecycleTests;

auto tLifecycleOpenCloseRepeatedly =
    test("WebViewLifecycle/openAndCloseRepeatedly") = []
{
    for (auto attempt = 0; attempt < repeatedOpenCount; ++attempt)
    {
        auto fix = LifecycleFixture {};
        check(fix.waitUntilReady(), "page " + std::to_string(attempt));
        check(fix.runScript("'alive'") == "alive");
    }
};

auto tLifecycleDestroyBeforeFirstNavigation =
    test("WebViewLifecycle/destroyBeforeTheFirstNavigationFinishes") = []
{
    {
        auto early = LifecycleFixture {};
    }

    Threads::runEventLoopUntil([] { return false; }, settleAfterTeardown);

    auto fix = LifecycleFixture {};
    check(fix.waitUntilReady());
    check(fix.runScript("'alive'") == "alive");
};

auto tLifecycleTwoAtOnce = test("WebViewLifecycle/twoWebViewsLiveSideBySide") = []
{
    auto first = LifecycleFixture {};
    auto second = LifecycleFixture {};

    check(first.waitUntilReady());
    check(second.waitUntilReady());
    check(first.runScript("'first'") == "first");
    check(second.runScript("'second'") == "second");
};

auto tBoundsPageSeesTheViewSize = test("WebViewBounds/pageSeesItsViewSize") = []
{
    auto fix = LifecycleFixture {};
    check(fix.waitUntilReady());
    check(fix.waitForPageSize(firstBounds));
};

auto tBoundsResizeReachesThePage = test("WebViewBounds/resizeReachesThePage") = []
{
    auto fix = LifecycleFixture {};
    check(fix.waitUntilReady());
    check(fix.waitForPageSize(firstBounds));

    fix.webView.setBounds(secondBounds);
    check(fix.waitForPageSize(secondBounds));

    fix.webView.setBounds(firstBounds);
    check(fix.waitForPageSize(firstBounds));
};

auto tPageMessageStringBody = test("PageMessage/stringBodyArrivesUnchanged") = []
{
    auto fix = LifecycleFixture {};
    check(fix.waitUntilReady());
    check(fix.postAndReceive("'hello'") == "hello");
    check(fix.postAndReceive("'{\"type\":\"ready\"}'") == R"({"type":"ready"})");
};

auto tPageMessageObjectBody = test("PageMessage/objectBodyArrivesAsJson") = []
{
    auto fix = LifecycleFixture {};
    check(fix.waitUntilReady());

    auto body = Miro::Json::parse(fix.postAndReceive("({type: 'ready', count: 2})"));
    check(body.isObject());
    check(body["type"].asString() == "ready");
    check(body["count"].asInteger() == 2);
};

auto tPageMessageArrayBody = test("PageMessage/arrayBodyArrivesAsJson") = []
{
    auto fix = LifecycleFixture {};
    check(fix.waitUntilReady());

    auto body = Miro::Json::parse(fix.postAndReceive("[1, 'two']"));
    check(body.isArray());
};

auto tPageMessageUnknownName = test("PageMessage/unregisteredNameIsIgnored") = []
{
    auto fix = LifecycleFixture {};
    check(fix.waitUntilReady());

    fix.runScript("try { window.webkit.messageHandlers.nobody.postMessage('x') } "
                  "catch (e) {} 'ok'");
    check(fix.postAndReceive("'after'") == "after");
    check(fix.received.size() == 1);
};
