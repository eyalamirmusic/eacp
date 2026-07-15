#include "Common.h"

#include <cmath>

// Drives View::renderToImageAsync end to end on a real WebView: the page paints
// a known colour, the async snapshot is awaited with waitFor (the pattern a unit
// test uses instead of a production sync path), and the resulting pixels are
// checked.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
bool near(const Color& c, int r, int g, int b, int tolerance = 12)
{
    auto within = [&](float channel, int target)
    { return std::abs((int) std::lround(channel * 255.f) - target) <= tolerance; };

    return within(c.r, r) && within(c.g, g) && within(c.b, b);
}

// Solid red page. A short setTimeout defers the ready signal to let layout
// settle -- requestAnimationFrame would not fire for an off-screen (headless)
// web view, but timers do.
const std::string pageHtml = R"HTML(<!doctype html><html><head><style>
  html,body{margin:0;height:100%;background:#e01010}
</style></head><body><script>
  setTimeout(function () {
    window.webkit.messageHandlers.ready.postMessage('ready');
  }, 50);
</script></body></html>)HTML";

struct Fixture
{
    WebView webView {};
    Window window {};
    bool ready = false;

    Fixture()
    {
        window.setContentView(webView);
        webView.setBounds({0.f, 0.f, 120.f, 80.f});
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.loadHTML(pageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         eacp::Time::MS {10000}));
    }
};
} // namespace

// renderToImageAsync folds the WebView's page into the snapshot; the whole image
// is the page's red background.
auto tAsyncSnapshotCapturesWebContent =
    test("RenderToImageAsync/capturesWebContent") = []
{
    auto fix = Fixture {};

    auto image = fix.webView.renderToImageAsync(1.f).waitFor(eacp::Time::MS {10000});

    check(image.isValid());
    check(image.width() == 120);
    check(image.height() == 80);
    check(near(image.at(60, 40), 224, 16, 16)); // page red at the centre
};

// A subtree with no web content still resolves (immediately) through the async
// path, matching the synchronous renderToImage.
auto tAsyncResolvesWithoutWebContent =
    test("RenderToImageAsync/resolvesWithoutWebContent") = []
{
    struct Solid final : View
    {
        Solid() { setBounds({0.f, 0.f, 20.f, 20.f}); }

        void paint(Context& g) override
        {
            g.setColor({0.f, 0.f, 1.f, 1.f});
            g.fillRect(getLocalBounds());
        }
    };

    auto view = Solid {};
    auto image = view.renderToImageAsync(1.f).waitFor(eacp::Time::MS {10000});

    check(image.isValid());
    check(image.width() == 20);
};
