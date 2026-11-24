#include "Common.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/GPU/GPU.h>
#include <eacp/WebView/WebView/WebViewSurface-Linux.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>

// The WPE page on screen: a WebView in a shown window presents through its
// WebViewSurface child, and what reached the swapchain is read back out of the
// drawable itself. Its own binary, with a main that is not headless (the
// WebViewTests main forces headless, which on Linux means no window system),
// and the eacp-linux-display lock the Present group takes. Without a display
// server every case skips, which EACP_REQUIRE_DISPLAY=1 turns into a failure.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace WebViewPresentTests
{
bool displayIsNamed()
{
    if (Apps::getAppEnvironment().headless)
        return false;

    auto requested = getEnvValue("EACP_WINDOW_SYSTEM");
    std::transform(requested.begin(),
                   requested.end(),
                   requested.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });

    auto wayland = !getEnvValue("WAYLAND_DISPLAY").empty()
                   || !getEnvValue("WAYLAND_SOCKET").empty();
    auto x11 = !getEnvValue("DISPLAY").empty();

    if (requested == "x11")
        return x11;

    if (requested == "wayland")
        return wayland;

    return wayland || x11;
}

bool cannotPresent()
{
    if (!displayIsNamed())
    {
        check(getEnvValue("EACP_REQUIRE_DISPLAY") != "1",
              "EACP_REQUIRE_DISPLAY=1 but no display server was reachable");
        return true;
    }

    if (!GPU::Device::shared().isValid())
    {
        check(getEnvValue("EACP_REQUIRE_GPU") != "1",
              "EACP_REQUIRE_GPU=1 but no Vulkan device came up");
        return true;
    }

    return false;
}

constexpr auto presentTimeout = Time::MS {5000};

std::string solidPage(const std::string& background)
{
    return R"HTML(<!doctype html><html><head><style>
  html,body{margin:0;height:100%}
  html{background:)HTML"
           + background + R"HTML(}
</style></head><body><script>
  requestAnimationFrame(function () {
    requestAnimationFrame(function () {
      window.webkit.messageHandlers.ready.postMessage('ready');
    });
  });
</script></body></html>)HTML";
}

const std::string animatedPage = R"HTML(<!doctype html><html><head><style>
  html,body{margin:0;height:100%}
</style></head><body><script>
  var hue = 0;
  function tick() {
    hue = (hue + 7) % 360;
    document.body.style.background = 'hsl(' + hue + ',80%,50%)';
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
  window.webkit.messageHandlers.ready.postMessage('ready');
</script></body></html>)HTML";

WindowOptions windowSized(int width, int height)
{
    auto options = WindowOptions {};
    options.title = "eacp WebView present tests";
    options.width = width;
    options.height = height;
    return options;
}

bool near(int value, int target, int tolerance = 6)
{
    return std::abs(value - target) <= tolerance;
}

bool matchesPixels(int reported, float points, float scale)
{
    auto expected = (int) std::lround(points * scale);
    return reported >= expected - 1 && reported <= expected + 1;
}

// B, G, R, A, premultiplied: the drawable's own byte order.
bool centreIs(const detail::PresentedPixels& pixels, int b, int g, int r, int a)
{
    const auto& centre = pixels.centre;

    return near(centre[0], b) && near(centre[1], g) && near(centre[2], r)
           && near(centre[3], a);
}

std::string describe(const std::string& what,
                     const std::optional<detail::PresentedPixels>& pixels)
{
    if (!pixels.has_value())
        return what + " (nothing was read back)";

    const auto& centre = pixels->centre;

    return what + " (read " + std::to_string(pixels->width) + "x"
           + std::to_string(pixels->height) + ", BGRA " + std::to_string(centre[0])
           + "," + std::to_string(centre[1]) + "," + std::to_string(centre[2]) + ","
           + std::to_string(centre[3]) + ")";
}

struct ShownPage
{
    explicit ShownPage(const std::string& html,
                       WebView::Options options = {},
                       int width = 320,
                       int height = 240)
        : webView(options)
        , window(windowSized(width, height))
    {
        content.addSubview(webView);
        webView.setBounds({0.f, 0.f, (float) width, (float) height});

        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.loadHTML(html);

        window.setContentView(content);
        window.setVisible(true);

        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout),
              "the page never loaded");
    }

    detail::WebViewSurface& surface()
    {
        for (auto* child: webView.getSubviews())
            if (auto* found = dynamic_cast<detail::WebViewSurface*>(child))
                return *found;

        check(false, "the WebView has no WebViewSurface child");
        std::abort();
    }

    bool waitForPresents(int count)
    {
        return Threads::runEventLoopUntil(
            [&] { return surface().presentedFrames() >= count; }, presentTimeout);
    }

    // Reads frames back until one satisfies `accept`, since the page may still
    // be settling when the first is read.
    std::optional<detail::PresentedPixels> readBackUntil(
        const std::function<bool(const detail::PresentedPixels&)>& accept)
    {
        auto last = std::optional<detail::PresentedPixels> {};

        Threads::runEventLoopUntil(
            [&]
            {
                auto pixels = surface().lastPresentedPixels();

                if (pixels.has_value())
                {
                    last = pixels;

                    if (accept(*pixels))
                        return true;
                }

                if (!pixels.has_value() && surface().isReadBackPending())
                    return false;

                surface().readBackNextFrame();
                return false;
            },
            presentTimeout);

        return last;
    }

    View content;
    WebView webView;
    Window window;
    bool ready = false;
};
} // namespace WebViewPresentTests

using namespace WebViewPresentTests;

auto tSolidPageReachesTheSwapchain =
    test("WebViewPresent/aSolidPageReachesTheSwapchain") = []
{
    if (cannotPresent())
        return;

    auto page = ShownPage {solidPage("rgb(0,128,255)")};

    check(page.waitForPresents(1), "no frame of the page was presented");

    auto pixels = page.readBackUntil([](const auto& read)
                                     { return centreIs(read, 255, 128, 0, 255); });

    check(pixels.has_value() && centreIs(*pixels, 255, 128, 0, 255),
          describe("the swapchain does not hold the page's colour", pixels));

    auto scale = page.surface().backingScale();
    check(pixels.has_value() && matchesPixels(pixels->width, 320.f, scale)
              && matchesPixels(pixels->height, 240.f, scale),
          describe("the drawable is not the WebView's size", pixels));
};

auto tTransparentPageStaysPremultiplied =
    test("WebViewPresent/aTransparentPageStaysPremultiplied") = []
{
    if (cannotPresent())
        return;

    auto options = WebView::Options {};
    options.transparentBackground = true;

    auto page = ShownPage {solidPage("rgba(255,0,0,0.5)"), options};

    check(page.waitForPresents(1), "no frame of the page was presented");

    auto pixels = page.readBackUntil([](const auto& read)
                                     { return centreIs(read, 0, 0, 128, 128); });

    check(pixels.has_value() && centreIs(*pixels, 0, 0, 128, 128),
          describe("half-transparent red did not arrive premultiplied", pixels));
};

auto tResizeReachesTheSwapchain =
    test("WebViewPresent/aResizeAfterTheFirstFrameReachesTheSwapchain") = []
{
    if (cannotPresent())
        return;

    auto page = ShownPage {solidPage("rgb(255,0,0)")};

    check(page.waitForPresents(1), "no frame of the page was presented");

    page.webView.setBounds({0.f, 0.f, 200.f, 150.f});

    auto pixels = page.readBackUntil(
        [&](const auto& read)
        {
            auto scale = page.surface().backingScale();
            return matchesPixels(read.width, 200.f, scale)
                   && matchesPixels(read.height, 150.f, scale)
                   && centreIs(read, 0, 0, 255, 255);
        });

    auto scale = page.surface().backingScale();
    check(pixels.has_value() && matchesPixels(pixels->width, 200.f, scale)
              && matchesPixels(pixels->height, 150.f, scale),
          describe("the swapchain never followed the WebView's new size", pixels));
    check(pixels.has_value() && centreIs(*pixels, 0, 0, 255, 255),
          describe("the resized page lost its colour", pixels));
};

auto tHideAndShowPresentsAgain =
    test("WebViewPresent/hidingAndShowingPresentsAgain") = []
{
    if (cannotPresent())
        return;

    auto page = ShownPage {solidPage("rgb(0,255,0)")};

    check(page.waitForPresents(1), "no frame of the page was presented");

    page.webView.setVisible(false);

    check(Threads::runEventLoopUntil([&] { return !page.surface().canPresent(); },
                                     presentTimeout),
          "a hidden WebView kept its surface");

    page.webView.setVisible(true);

    check(Threads::runEventLoopUntil([&] { return page.surface().canPresent(); },
                                     presentTimeout),
          "a WebView shown again never got its surface back");

    auto pixels = page.readBackUntil([](const auto& read)
                                     { return centreIs(read, 0, 255, 0, 255); });

    check(pixels.has_value() && centreIs(*pixels, 0, 255, 0, 255),
          describe("the page was not presented again after showing", pixels));
};

auto tStaticPageGoesIdle =
    test("WebViewPresent/aStaticPageStopsAskingForFrames") = []
{
    if (cannotPresent())
        return;

    auto page = ShownPage {solidPage("rgb(40,40,40)")};

    check(page.waitForPresents(1), "no frame of the page was presented");

    check(Threads::runEventLoopUntil([&] { return !page.surface().isContinuous(); },
                                     presentTimeout),
          "a static page kept its frame loop running");

    auto settled = page.surface().presentedFrames();
    Threads::runEventLoopFor(Time::MS {600});

    check(!page.surface().isContinuous(), "a static page restarted its frame loop");
    check(page.surface().presentedFrames() == settled,
          "a static page went on presenting new frames");
};

auto tAnimatedPageKeepsPresenting =
    test("WebViewPresent/anAnimatedPageKeepsPresenting") = []
{
    if (cannotPresent())
        return;

    auto page = ShownPage {animatedPage};

    check(page.waitForPresents(1), "no frame of the page was presented");

    auto before = page.surface().presentedFrames();
    Threads::runEventLoopFor(Time::MS {1000});
    auto presented = page.surface().presentedFrames() - before;

    // Sixty is the figure on a 60 Hz output; lavapipe under a busy CI runner
    // manages far fewer, and a pipeline that stalled after one frame none.
    check(presented >= 5, "an animated page presented fewer than 5 frames in 1s");
    check(page.surface().isContinuous(), "an animated page let its loop stop");
};

namespace
{
int webViewPresentArgCount = 0;
char** webViewPresentArgValues = nullptr;
int webViewPresentExitCode = 0;

void runWebViewPresentTests()
{
    webViewPresentExitCode =
        nano::run(webViewPresentArgCount, webViewPresentArgValues);
}
} // namespace

// Apps::run wraps it: runEventLoopUntil needs a bootstrapped loop.
int main(int argc, char* argv[])
{
    webViewPresentArgCount = argc;
    webViewPresentArgValues = argv;

    Apps::run(runWebViewPresentTests);

    return webViewPresentExitCode;
}
