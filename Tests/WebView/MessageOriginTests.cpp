#include "Common.h"
// Page -> native messages are checked against their sender before any handler
// runs (WebView::Options::trustedOrigins). The view's own content keeps its
// handlers; a page from any other origin, or a foreign frame inside the view's
// own page, posts into nothing. "*" restores delivery from everywhere.
//
// The page reports that it has posted through a JS global read back with
// callJS, which needs no handler, so a page whose messages drop can still say
// it ran.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace MessageOriginTests
{
// Long enough for a message that was going to arrive to have arrived: the
// round trip the page's own flag took is already behind us when this starts.
constexpr auto deliveryGrace = Time::MS {500};

constexpr auto remoteBase = "https://example.com/";

const std::string mainFramePostingPage = R"HTML(<!doctype html>
<html><body><script>
  window.webkit.messageHandlers.probe.postMessage('main');
  window.__posted = true;
</script></body></html>)HTML";

const std::string windowControlPostingPage = R"HTML(<!doctype html>
<html><body><script>
  window.webkit.messageHandlers.__eacpWindowControl.postMessage('close');
  window.__posted = true;
</script></body></html>)HTML";

std::string pageWithFrame(const std::string& frameAttributes)
{
    return "<!doctype html><html><body><iframe " + frameAttributes
           + " onload=\"window.__posted = true\"></iframe></body></html>";
}

const auto sandboxedFrame =
    std::string {"sandbox=\"allow-scripts\" srcdoc=\"<script>"
                 "window.webkit.messageHandlers.probe.postMessage('sandboxed')"
                 "</script>\""};

const auto dataFrame =
    std::string {"src=\"data:text/html,<script>"
                 "window.webkit.messageHandlers.probe.postMessage('data')"
                 "</script>\""};

WebView::Options trusting(Vector<std::string> origins)
{
    auto options = WebView::Options {};
    options.trustedOrigins = std::move(origins);
    return options;
}

WebView::Options servingScheme(const std::string& scheme, const std::string& html)
{
    auto options = WebView::Options {};
    options.schemes[scheme] = [html](std::string_view)
    {
        auto response = ResourceResponse {};
        response.mimeType = "text/html";
        response.data.assign(html.begin(), html.end());
        return std::optional {response};
    };

    // WebView2 fails any environment that shares a user-data folder with one
    // registering a different set of custom schemes (ERROR_INVALID_STATE), and
    // the plain views the other cases create use the shared folder, so this
    // one needs its own (see StreamingPumpTests).
    options.userDataFolderSuffix = "messageorigin-" + scheme;
    return options;
}

struct OriginFixture
{
    WebView webView;
    Window window {};
    Vector<std::string> received;

    explicit OriginFixture(WebView::Options options = {})
        : webView(std::move(options))
    {
        window.setContentView(webView);
        listenOn("probe");
    }

    void listenOn(const std::string& name)
    {
        webView.addScriptMessageHandler(
            name, [this](const std::string& body) { received.add(body); });
    }

    void loadHTML(const std::string& html, const std::string& baseURL = {})
    {
        webView.loadHTML(html, baseURL);
        waitUntilPosted();
    }

    void loadURL(const std::string& url)
    {
        webView.loadURL(url);
        waitUntilPosted();
    }

    void waitUntilPosted()
    {
        auto posted = [this]
        {
            return webView.callJS("window.__posted === true ? 'yes' : 'no'")
                       .waitFor(webViewResultTimeout)
                   == "yes";
        };
        check(Threads::runEventLoopUntil(posted, firstNavigationTimeout));
    }

    bool deliveredWithin(Time::MS timeout)
    {
        return Threads::runEventLoopUntil([this] { return !received.empty(); },
                                          timeout);
    }

    bool delivered() { return deliveredWithin(webViewResultTimeout); }
    bool dropped() { return !deliveredWithin(deliveryGrace); }

    std::string firstReceived() const
    {
        return received.empty() ? std::string {} : received.front();
    }
};

auto tInlineDocument = test("MessageOrigin/inlineDocumentReachesHandler") = []
{
    auto fix = OriginFixture {};
    fix.loadHTML(mainFramePostingPage);
    check(fix.delivered());
    check(fix.firstReceived() == "main");
};

auto tOwnScheme = test("MessageOrigin/ownSchemeReachesHandler") = []
{
    auto fix = OriginFixture {servingScheme("eacporigin", mainFramePostingPage)};
    fix.loadURL("eacporigin://local/index.html");
    check(fix.delivered());
};

auto tRemoteOrigin = test("MessageOrigin/remoteOriginIsDropped") = []
{
    auto fix = OriginFixture {};
    fix.loadHTML(mainFramePostingPage, remoteBase);
    check(fix.dropped());
};

auto tUnlistedOrigin = test("MessageOrigin/unlistedOriginIsDropped") = []
{
    auto fix = OriginFixture {trusting({"https://example.org"})};
    fix.loadHTML(mainFramePostingPage, remoteBase);
    check(fix.dropped());
};

auto tListedOrigin = test("MessageOrigin/listedOriginReachesHandler") = []
{
    auto fix = OriginFixture {trusting({"https://example.com"})};
    fix.loadHTML(mainFramePostingPage, remoteBase);
    check(fix.delivered());
};

auto tSandboxedFrame = test("MessageOrigin/sandboxedFrameInOwnPageIsDropped") = []
{
    auto fix = OriginFixture {};
    fix.loadHTML(pageWithFrame(sandboxedFrame));
    check(fix.dropped());
};

auto tDataFrame = test("MessageOrigin/dataFrameInOwnPageIsDropped") = []
{
    auto fix = OriginFixture {};
    fix.loadHTML(pageWithFrame(dataFrame));
    check(fix.dropped());
};

auto tWildcardRemote = test("MessageOrigin/wildcardTrustsRemoteOrigin") = []
{
    auto fix = OriginFixture {trusting({"*"})};
    fix.loadHTML(mainFramePostingPage, remoteBase);
    check(fix.delivered());
};

// WKWebView exposes handlers to every frame; WebView2 delivers only the top
// document's messages, so a frame has nothing to restore there.
auto tWildcardFrame = test("MessageOrigin/wildcardTrustsForeignFrame") = []
{
    if (!Platform::isApple())
        return;

    auto fix = OriginFixture {trusting({"*"})};
    fix.loadHTML(pageWithFrame(sandboxedFrame));
    check(fix.delivered());
};

// The probe replaces the framework's own handler under the same name, so what
// is observed is exactly whether the window-control bridge would have run.
auto tRemoteWindowControl =
    test("MessageOrigin/windowControlFromRemoteOriginIsDropped") = []
{
    auto fix = OriginFixture {};
    fix.listenOn("__eacpWindowControl");
    fix.loadHTML(windowControlPostingPage, remoteBase);
    check(fix.dropped());
};

auto tOwnWindowControl =
    test("MessageOrigin/windowControlFromInlineDocumentArrives") = []
{
    auto fix = OriginFixture {};
    fix.listenOn("__eacpWindowControl");
    fix.loadHTML(windowControlPostingPage);
    check(fix.delivered());
    check(fix.firstReceived() == "close");
};
} // namespace MessageOriginTests
