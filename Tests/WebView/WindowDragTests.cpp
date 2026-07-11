#include "Common.h"
// window-drag.js (auto-injected) exposes window.__eacpResolveAppRegion, which
// classifies a point as drag / no-drag. These assert that classification on a
// real WKWebView -- which also pins down that the `--eacp-app-region` custom
// property is readable via getComputedStyle (the native one is not).

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;
using namespace std::chrono_literals;

namespace
{
// Drag bar with an opt-out button and an unmarked label, plus an unmarked
// sibling. `ready` gates queries until document-start injection has run.
const std::string pageHtml = R"HTML(
<!doctype html>
<html>

<head>
<style>
  #bar { --eacp-app-region: drag; }
  #btn { --eacp-app-region: no-drag; }
</style>
</head>

<body>
  <div id="bar">
    <button id="btn">x</button>
    <span id="label">title</span>
  </div>
  <div id="outside">body</div>
  <script>window.webkit.messageHandlers.ready.postMessage('ready');</script>
</body>

</html>
)HTML";

struct Fixture
{
    WebView webView {};
    Window window {};
    bool ready = false;

    Fixture()
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.loadHTML(pageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; }, 10s));
    }

    std::string regionOf(const std::string& selector)
    {
        auto script = "window.__eacpResolveAppRegion(document.querySelector('"
                      + selector + "'))";
        return webView.callJS(script).waitFor(10s);
    }
};
} // namespace

auto tDragRegionResolves = test("WindowDrag/dragRegionResolvesToDrag") = []
{
    auto fix = Fixture {};
    check(fix.regionOf("#bar") == "drag");
};

auto tNoDragOptsOut = test("WindowDrag/noDragDescendantOptsOut") = []
{
    auto fix = Fixture {};
    check(fix.regionOf("#btn") == "no-drag");
};

auto tUnmarkedChildInherits = test("WindowDrag/unmarkedChildInheritsDrag") = []
{
    auto fix = Fixture {};
    // Inherits -> the whole bar is a handle, not just its background.
    check(fix.regionOf("#label") == "drag");
};

auto tOutsideIsNotDraggable = test("WindowDrag/unmarkedRegionIsEmpty") = []
{
    auto fix = Fixture {};
    check(fix.regionOf("#outside").empty());
};

// Regression: a non-string message body (a bare number) once threw in
// didReceiveScriptMessage's NSJSONSerialization and aborted the app. Without the
// isValidJSONObject guard this crashes the process; with it the handler fires
// with an empty body.
struct NumberMessageProbe
{
    WebView webView {};
    Window window {};
    bool called = false;
    std::string body = "unset";

    NumberMessageProbe()
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler("numberProbe",
                                        [this](const std::string& received)
                                        {
                                            called = true;
                                            body = received;
                                        });
        webView.loadHTML("<!doctype html><html><body><script>"
                         "window.webkit.messageHandlers.numberProbe.postMessage(1);"
                         "</script></body></html>");
        check(Threads::runEventLoopUntil([this] { return called; }, 10s));
    }
};

auto tNumberBodyDoesNotCrash = test("WindowDrag/numberMessageBodyDoesNotCrash") = []
{
    auto probe = NumberMessageProbe {};
    check(probe.called); // handler fired -- the app did not abort
    check(probe.body.empty()); // invalid JSON top level -> empty body, no throw
};

// Why the marker is a custom property and not the standard -webkit-app-region:
// WKWebView drops the unknown native prop, so getComputedStyle can't read it,
// while the custom property IS exposed. Chromium (WebView2) supports
// app-region natively, so there the standard prop reads back too -- the custom
// property is the one readable on BOTH engines, which is why window-drag.js
// keys on it. If a future WebKit exposes -webkit-app-region, the Apple branch
// fails -- prompting a simplification.
struct MarkerProbe
{
    WebView webView {};
    Window window {};
    bool ready = false;

    MarkerProbe()
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.loadHTML(
            "<!doctype html><html><head><style>"
            "#m { -webkit-app-region: drag; --eacp-app-region: drag; }"
            "</style></head><body><div id=\"m\">x</div>"
            "<script>window.webkit.messageHandlers.ready.postMessage('r');</script>"
            "</body></html>");
        check(Threads::runEventLoopUntil([this] { return ready; }, 10s));
    }

    std::string computed(const std::string& prop)
    {
        return webView
            .callJS("getComputedStyle(document.getElementById('m'))"
                    ".getPropertyValue('"
                    + prop + "').trim()")
            .waitFor(10s);
    }
};

auto tCustomPropReadableUnlikeWebkit =
    test("WindowDrag/customPropReadableUnlikeWebkitAppRegion") = []
{
    auto p = MarkerProbe {};

    if (Platform::isWindows())
        check(p.computed("-webkit-app-region") == "drag"); // Chromium: native
    else
        check(p.computed("-webkit-app-region").empty()); // WebKit: invisible

    check(p.computed("--eacp-app-region") == "drag"); // custom prop: readable
};
