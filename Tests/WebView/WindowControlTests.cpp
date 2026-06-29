// window-controls.js (auto-injected) exposes window.__eacpResolveWindowButton,
// which classifies an element as a caption-button role (minimize / maximize /
// close). These assert that classification on a real engine, plus the
// attributes the shim mirrors onto <html> so pages can drive their chrome
// from pure CSS: data-eacp-platform and data-eacp-maximized.

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/WebView/WebView.h>

#include <NanoTest/NanoTest.h>

#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;
using namespace std::chrono_literals;

namespace
{
constexpr auto firstNavigationTimeout = 30s;

// One button per role, a glyph child inside one of them (the custom property
// inherits, so a click landing on the glyph still resolves), and an unmarked
// sibling. `ready` gates queries until document-start injection has run.
const std::string pageHtml = R"HTML(
<!doctype html>
<html>

<head>
<style>
  #min   { --eacp-window-button: minimize; }
  #max   { --eacp-window-button: maximize; }
  #close { --eacp-window-button: close; }
</style>
</head>

<body>
  <button id="min"></button>
  <button id="max"><span id="glyph">x</span></button>
  <button id="close"></button>
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
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }

    std::string buttonOf(const std::string& selector)
    {
        auto script = "window.__eacpResolveWindowButton(document.querySelector('"
                      + selector + "'))";
        return webView.callJS(script).waitFor(10s);
    }
};
} // namespace

auto tRolesResolve = test("WindowControl/buttonRolesResolve") = []
{
    auto fix = Fixture {};
    check(fix.buttonOf("#min") == "minimize");
    check(fix.buttonOf("#max") == "maximize");
    check(fix.buttonOf("#close") == "close");
};

auto tGlyphChildResolves = test("WindowControl/childOfButtonResolves") = []
{
    auto fix = Fixture {};
    // Inherits -> a glyph inside the button is still a hit.
    check(fix.buttonOf("#glyph") == "maximize");
};

auto tUnmarkedIsEmpty = test("WindowControl/unmarkedElementIsEmpty") = []
{
    auto fix = Fixture {};
    check(fix.buttonOf("#outside").empty());
};

auto tPlatformAttribute = test("WindowControl/platformAttributeMirrorsNative") = []
{
    auto fix = Fixture {};
    auto attribute =
        fix.webView
            .callJS("document.documentElement.getAttribute('data-eacp-platform')")
            .waitFor(10s);
    check(attribute == (Platform::isWindows() ? "windows" : "mac"));
};

auto tMaximizedAttribute = test("WindowControl/maximizedAttributeTracksNative") = []
{
    // Native reports maximize toggles through __eacpSetMaximized; the page
    // keys its restore glyph off the resulting <html> attribute.
    auto fix = Fixture {};

    auto hasAttribute = [&fix]
    {
        return fix.webView
            .callJS("document.documentElement.hasAttribute('data-eacp-maximized')"
                    " ? 'yes' : 'no'")
            .waitFor(10s);
    };

    check(hasAttribute() == "no");

    fix.webView.callJS("window.__eacpSetMaximized(true)").waitFor(10s);
    check(hasAttribute() == "yes");

    fix.webView.callJS("window.__eacpSetMaximized(false)").waitFor(10s);
    check(hasAttribute() == "no");
};
