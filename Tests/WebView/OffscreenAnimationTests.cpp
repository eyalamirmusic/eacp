#include "Common.h"

// Options::driveOffscreenAnimation keeps a requestAnimationFrame loop running
// while the view is off-screen. Without it, rAF never fires off-screen, so a
// canvas that paints on rAF stays blank; with it, rAF is redirected onto a timer
// and the canvas paints — the difference the two tests below capture.
//
// Both read the answer out of the page — the frame counter, and the canvas
// centre via getImageData — rather than through renderToImageAsync. Whether rAF
// fires is something the page can report directly; routing it through
// -[WKWebView takeSnapshotWithConfiguration:] only added a second, far slower
// subsystem, and one whose completion handler does not arrive within ten seconds
// often enough to fail on a loaded macOS CI runner, where the tests are headless
// so the page is permanently hidden and its web process throttled.
// RenderToImageAsyncTests covers the snapshot path itself.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
// A red page with a full-bleed canvas that an rAF loop paints green. Until rAF
// fires, the canvas is transparent and the red body shows through. A timer
// (which fires off-screen even when rAF does not) signals ready, so the test can
// read the page regardless of whether the animation ran.
//
// The loop chains a fixed number of frames and then stops, signalling painted:
// chaining is what proves rAF keeps firing off-screen rather than landing one
// frame and stalling.
const std::string pageHtml = R"HTML(<!doctype html><html><head><style>
  html,body{margin:0;height:100%;background:#e01010}
  canvas{display:block;width:100%;height:100%}
</style></head><body><canvas id="c"></canvas><script>
  var c = document.getElementById('c');
  c.width = 60; c.height = 40;
  var ctx = c.getContext('2d');
  window.__frames = 0;
  function post(name){ window.webkit.messageHandlers[name].postMessage(name); }
  function frame(){ ctx.fillStyle = '#10c020'; ctx.fillRect(0,0,60,40);
                    if (++window.__frames < 3) requestAnimationFrame(frame);
                    else post('painted'); }
  window.__centre = function () {
    var d = ctx.getImageData(30, 20, 1, 1).data;
    return d[0] + ',' + d[1] + ',' + d[2] + ',' + d[3];
  };
  requestAnimationFrame(frame);
  setTimeout(function () { post('ready'); }, 50);
</script></body></html>)HTML";

// The canvas centre once the loop has painted it (#10c020, opaque), and while
// nothing has drawn into it at all. Exact rather than approximate: these come
// from getImageData, which reads back the very bytes fillRect wrote, with no
// encode / colour-space round trip to blur them.
constexpr auto paintedCentre = "16,192,32,255";
constexpr auto untouchedCentre = "0,0,0,0";

// How long to let an off-screen rAF loop prove itself absent before concluding
// it is frozen. Only ever spent in full on the frozen path — the case that
// expects frames waits for the signal instead.
constexpr auto frozenSettleTime = eacp::Time::MS {250};

struct Fixture
{
    WebView webView;
    Window window {};
    bool ready = false;
    bool painted = false;

    explicit Fixture(bool driveOffscreen)
        : webView(makeOptions(driveOffscreen))
    {
        window.setContentView(webView);
        webView.setBounds({0.f, 0.f, 60.f, 40.f});
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.addScriptMessageHandler(
            "painted", [this](const std::string&) { painted = true; });
        webView.loadHTML(pageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }

    // Waits for the rAF loop to land all three frames. Only for the cases that
    // expect rAF to run at all: where it stays frozen off-screen, painted never
    // arrives. A real timeout rather than a fixed sleep because a hidden page's
    // timers are throttled to roughly 1 Hz — and the shim rAF rides those
    // timers — so three frames can take a few seconds.
    void waitForPaint()
    {
        check(Threads::runEventLoopUntil([this] { return painted; },
                                         webViewResultTimeout));
    }

    static WebView::Options makeOptions(bool driveOffscreen)
    {
        auto options = WebView::Options {};
        options.driveOffscreenAnimation = driveOffscreen;
        return options;
    }

    std::string read(const std::string& script)
    {
        return webView.callJS(script).waitFor(webViewResultTimeout);
    }

    std::string centre() { return read("window.__centre()"); }
    std::string frameCount() { return read("String(window.__frames)"); }
};
} // namespace

// With the flag, the rAF loop runs off-screen: the frames chain to completion
// and the canvas ends up painted green.
auto tDrivesAnimationOffscreen = test("OffscreenAnimation/paintsWhenEnabled") = []
{
    auto fix = Fixture {/*driveOffscreen*/ true};
    fix.waitForPaint();
    check(fix.frameCount() == "3");
    check(fix.centre() == paintedCentre);
};

// Without the flag, whether an off-screen rAF loop keeps running is left to the
// platform, and the two desktop backends genuinely differ:
//   - WKWebView has no display link off-screen, so rAF never fires: the canvas
//     is never drawn into and the red body shows through.
//   - WebView2 is composition-hosted and always reports itself visible (so that
//     renderToImageAsync can snapshot it at all), so its rAF keeps firing and
//     paints the canvas green even off-screen — the flag is redundant there.
// Either way the default is not "paints regardless": on macOS it establishes
// that paintsWhenEnabled's green comes from the flag, and on Windows it pins the
// native-rAF behaviour the flag harmlessly mirrors.
auto tAnimationDefaultMatchesPlatform =
    test("OffscreenAnimation/frozenByDefault") = []
{
    auto fix = Fixture {/*driveOffscreen*/ false};

    if (Platform::isWindows())
    {
        fix.waitForPaint();
        check(fix.centre() == paintedCentre); // native rAF paints it
    }
    else
    {
        Threads::runEventLoopFor(frozenSettleTime);
        check(fix.frameCount() == "0"); // rAF never fired
        check(fix.centre() == untouchedCentre); // so nothing drew into it
    }
};
