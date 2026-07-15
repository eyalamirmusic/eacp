#include "Common.h"

#include <cmath>

// Options::driveOffscreenAnimation keeps a requestAnimationFrame loop running
// while the view is off-screen. Without it, rAF never fires off-screen, so a
// canvas that paints on rAF stays blank; with it, rAF is redirected onto a timer
// and the canvas paints — the difference the two tests below capture.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
bool near(const Color& c, int r, int g, int b, int tolerance = 16)
{
    auto within = [&](float channel, int target)
    { return std::abs((int) std::lround(channel * 255.f) - target) <= tolerance; };

    return within(c.r, r) && within(c.g, g) && within(c.b, b);
}

// A red page with a full-bleed canvas that an rAF loop paints green. Until rAF
// fires, the canvas is transparent and the red body shows through. A timer (which
// fires off-screen even when rAF does not) signals ready, so the test can snapshot
// regardless of whether the animation ran.
const std::string pageHtml = R"HTML(<!doctype html><html><head><style>
  html,body{margin:0;height:100%;background:#e01010}
  canvas{display:block;width:100%;height:100%}
</style></head><body><canvas id="c"></canvas><script>
  var c = document.getElementById('c');
  c.width = 60; c.height = 40;
  var ctx = c.getContext('2d');
  function frame(){ ctx.fillStyle = '#10c020'; ctx.fillRect(0,0,60,40);
                    requestAnimationFrame(frame); }
  requestAnimationFrame(frame);
  setTimeout(function () {
    window.webkit.messageHandlers.ready.postMessage('ready');
  }, 150);
</script></body></html>)HTML";

struct Fixture
{
    WebView webView;
    Window window {};
    bool ready = false;

    explicit Fixture(bool driveOffscreen)
        : webView(makeOptions(driveOffscreen))
    {
        window.setContentView(webView);
        webView.setBounds({0.f, 0.f, 60.f, 40.f});
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.loadHTML(pageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         eacp::Time::MS {10000}));
        // Give the rAF loop a few ticks to paint (when it is running at all).
        Threads::runEventLoopFor(eacp::Time::MS {200});
    }

    static WebView::Options makeOptions(bool driveOffscreen)
    {
        auto options = WebView::Options {};
        options.driveOffscreenAnimation = driveOffscreen;
        return options;
    }

    Color centre()
    {
        auto image = webView.renderToImageAsync(1.f).waitFor(eacp::Time::MS {10000});
        check(image.isValid());
        return image.at(30, 20);
    }
};
} // namespace

// With the flag, the rAF loop runs off-screen and paints the canvas green.
auto tDrivesAnimationOffscreen = test("OffscreenAnimation/paintsWhenEnabled") = []
{
    auto fix = Fixture {/*driveOffscreen*/ true};
    check(near(fix.centre(), 16, 192, 32)); // #10c020, the painted green
};

// Without it, rAF never fires off-screen: the canvas stays transparent and the
// red page shows through. (Establishes that the flag is what makes the
// difference — not something that paints regardless.)
auto tLeavesAnimationFrozenByDefault =
    test("OffscreenAnimation/frozenByDefault") = []
{
    auto fix = Fixture {/*driveOffscreen*/ false};
    check(near(fix.centre(), 224, 16, 16)); // #e01010, the unpainted red body
};
