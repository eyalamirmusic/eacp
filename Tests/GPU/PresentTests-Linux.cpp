#include "Common.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Time.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

// Without a display server these skip, which ctest scores as a pass, so
// EACP_REQUIRE_DISPLAY=1 turns the skip into a failure.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool waylandIsNamed()
{
    return !getEnvValue("WAYLAND_DISPLAY").empty()
           || !getEnvValue("WAYLAND_SOCKET").empty();
}

bool x11IsNamed()
{
    return !getEnvValue("DISPLAY").empty();
}

// Not a connection attempt - making one is the window backend's job. The rule
// mirrors linuxChooseWindowSystem: eacp-gpu links no window system, so the
// test reads the same environment the backend does rather than asking it.
bool displayIsReachable()
{
    if (Apps::getAppEnvironment().headless)
        return false;

    auto requested = getEnvValue("EACP_WINDOW_SYSTEM");
    std::transform(requested.begin(),
                   requested.end(),
                   requested.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });

    if (requested == "x11")
        return x11IsNamed();

    if (requested == "wayland")
        return waylandIsNamed();

    return waylandIsNamed() || x11IsNamed();
}

bool noDisplay()
{
    if (displayIsReachable())
        return false;

    check(getEnvValue("EACP_REQUIRE_DISPLAY") != "1",
          "EACP_REQUIRE_DISPLAY=1 but no display server was reachable - "
          "every case in this file would otherwise have skipped and reported "
          "a pass");

    return true;
}

bool noDeviceOrDisplay()
{
    return !Device::shared().isValid() || noDisplay();
}

struct CountingView final : GPUView
{
    // Protected on GPUView; a test is that subclass.
    using GPUView::renderNow;

    void render(Frame& frame) override
    {
        ++renders;
        everyFrameWasValid = everyFrameWasValid && frame.isValid();

        auto pass = frame.beginPass({});

        lastWidth = pass.targetWidth();
        lastHeight = pass.targetHeight();
    }

    void update(Threads::FrameTime time) override
    {
        ++updates;
        lastTime = time;
    }

    int renders = 0;
    int updates = 0;
    bool everyFrameWasValid = true;
    int lastWidth = 0;
    int lastHeight = 0;
    Threads::FrameTime lastTime;
};

bool pumpUntil(Time::MS limit, const std::function<bool()>& done)
{
    constexpr auto slice = Time::MS {8};

    auto deadline = Time::Deadline {limit};

    while (!deadline.expired())
    {
        if (done())
            return true;

        Threads::runEventLoopFor(slice);
    }

    return done();
}

// Dozens of chances at 60 Hz, but seconds when nothing is ever presented.
constexpr auto presentTimeout = Time::MS {3000};

// Within a pixel: ViewSurface rounds bounds times scale to whole pixels
// without saying which way.
bool matchesPixels(int reported, float points, float scale)
{
    const auto expected = static_cast<int>(std::lround(points * scale));

    return reported >= expected - 1 && reported <= expected + 1;
}

Graphics::WindowOptions windowSized(int width, int height)
{
    auto options = Graphics::WindowOptions {};
    options.title = "eacp present tests";
    options.width = width;
    options.height = height;

    return options;
}

void showWith(Graphics::Window& window, Graphics::View& view)
{
    window.setContentView(view);
    window.setVisible(true);
}
} // namespace

auto tWindowPresentsAFrame = test("Present/aShownWindowPresentsAFrame") = []
{
    if (noDeviceOrDisplay())
        return;

    auto view = CountingView {};
    auto window = Graphics::Window {windowSized(320, 240)};
    showWith(window, view);

    const auto drew = pumpUntil(presentTimeout, [&] { return view.renders > 0; });

    check(drew, "no frame was presented within the timeout");
    check(view.everyFrameWasValid, "a drawable Frame reported itself invalid");
    check(view.lastWidth > 0);
    check(view.lastHeight > 0);
};

auto tContinuousKeepsGoing = test("Present/continuousModeKeepsPresenting") = []
{
    if (noDeviceOrDisplay())
        return;

    auto view = CountingView {};
    auto window = Graphics::Window {windowSized(320, 240)};
    showWith(window, view);

    view.setContinuous(true);

    const auto ran = pumpUntil(presentTimeout, [&] { return view.renders >= 3; });

    check(ran, "continuous mode stopped after fewer than three frames");

    // update() runs before the render it belongs to.
    check(view.updates >= view.renders - 1);

    view.setContinuous(false);

    const auto after = view.renders;
    pumpUntil(Time::MS {200}, [] { return false; });

    check(view.renders == after, "frames kept coming after setContinuous(false)");
};

// The cap divides the compositor's frame callbacks, and a tick that skips has
// to ask for the next callback itself: nothing else commits for it, and the
// loop would stop at the first skip.
auto tMaxFpsPacesContinuousMode = test("Present/maxFpsPacesContinuousMode") = []
{
    if (noDeviceOrDisplay())
        return;

    auto view = CountingView {};
    auto window = Graphics::Window {windowSized(320, 240)};
    showWith(window, view);

    view.setMaxFps(10);
    view.setContinuous(true);

    check(pumpUntil(presentTimeout, [&] { return view.renders > 0; }),
          "a capped continuous view never presented");

    const auto before = view.renders;

    pumpUntil(Time::MS {1500}, [] { return false; });

    const auto drawn = view.renders - before;

    view.setContinuous(false);

    // Fifteen is the figure; the bounds are wide enough for a compositor that
    // pauses and a machine that is busy, and narrow enough that an uncapped
    // 60 Hz (ninety) or a loop that stopped (zero) fails.
    check(drawn >= 6, "a 10 fps view presented fewer than 6 frames in 1.5s");
    check(drawn <= 30, "a 10 fps view presented more than 30 frames in 1.5s");
};

auto tOnDemandFrames = test("Present/renderNowAndRepaintEachPresentOneFrame") = []
{
    if (noDeviceOrDisplay())
        return;

    auto view = CountingView {};
    auto window = Graphics::Window {windowSized(320, 240)};
    showWith(window, view);

    check(pumpUntil(presentTimeout, [&] { return view.renders > 0; }),
          "no frame was presented within the timeout");

    const auto afterFirst = view.renders;

    view.renderNow();
    check(view.renders == afterFirst + 1, "renderNow() did not render exactly once");

    const auto afterRenderNow = view.renders;

    view.repaint();
    check(pumpUntil(presentTimeout, [&] { return view.renders > afterRenderNow; }),
          "repaint() never reached the view");

    check(view.renders == afterRenderNow + 1,
          "repaint() rendered more than one frame");
};

auto tResizeFollowsTheView = test("Present/resizeReachesTheSwapchain") = []
{
    if (noDeviceOrDisplay())
        return;

    auto content = Graphics::View {};
    auto view = CountingView {};

    content.addSubview(view);
    view.setBounds({0.f, 0.f, 160.f, 120.f});

    auto window = Graphics::Window {windowSized(320, 240)};
    showWith(window, content);

    check(pumpUntil(presentTimeout, [&] { return view.renders > 0; }),
          "no frame was presented within the timeout");

    const auto scale = view.backingScale();

    check(scale > 0.f);
    check(matchesPixels(view.lastWidth, 160.f, scale));
    check(matchesPixels(view.lastHeight, 120.f, scale));

    const auto before = view.renders;

    view.setBounds({0.f, 0.f, 200.f, 100.f});

    check(pumpUntil(presentTimeout,
                    [&]
                    {
                        return view.renders > before
                               && matchesPixels(
                                   view.lastWidth, 200.f, view.backingScale());
                    }),
          "the swapchain never followed the view's new size");

    check(matchesPixels(view.lastHeight, 100.f, view.backingScale()));
};

auto tVisibilityStopsAndResumes =
    test("Present/hidingStopsFramesAndShowingResumes") = []
{
    if (noDeviceOrDisplay())
        return;

    auto view = CountingView {};
    auto window = Graphics::Window {windowSized(320, 240)};
    showWith(window, view);

    view.setContinuous(true);

    check(pumpUntil(presentTimeout, [&] { return view.renders >= 2; }),
          "continuous mode never got going");

    view.setVisible(false);

    // A frame may still be in flight, so the count is sampled a turn later.
    pumpUntil(Time::MS {200}, [] { return false; });

    const auto whileHidden = view.renders;
    pumpUntil(Time::MS {300}, [] { return false; });

    check(view.renders == whileHidden, "a hidden view went on presenting");

    view.setVisible(true);

    check(pumpUntil(presentTimeout, [&] { return view.renders > whileHidden; }),
          "a view that was shown again never presented");
};

auto tSnapshotWorksWithASwapchain =
    test("Present/renderToImageWorksWhilePresenting") = []
{
    if (noDeviceOrDisplay())
        return;

    auto view = CountingView {};
    auto window = Graphics::Window {windowSized(320, 240)};
    showWith(window, view);

    check(pumpUntil(presentTimeout, [&] { return view.renders > 0; }),
          "no frame was presented within the timeout");

    auto image = view.renderToImage(1.f);

    check(image.isValid(), "the off-screen snapshot failed while presenting");
    check(image.width() == 320);
    check(image.height() == 240);

    const auto after = view.renders;
    view.renderNow();

    check(view.renders == after + 1, "presenting stopped after a snapshot");
    check(view.everyFrameWasValid);
};

// The VkSwapchainKHR has to go before the wl_surface it was made from.
auto tTeardownAndRebuild = test("Present/aWindowCanBeReplaced") = []
{
    if (noDeviceOrDisplay())
        return;

    {
        auto first = CountingView {};
        auto window = Graphics::Window {windowSized(320, 240)};
        showWith(window, first);

        check(pumpUntil(presentTimeout, [&] { return first.renders > 0; }),
              "the first window never presented");
    }

    // Let the loop turn so anything the teardown deferred has run.
    pumpUntil(Time::MS {100}, [] { return false; });

    auto second = CountingView {};
    auto window = Graphics::Window {windowSized(256, 192)};
    showWith(window, second);

    check(pumpUntil(presentTimeout, [&] { return second.renders > 0; }),
          "a window built after one was destroyed never presented");
    check(second.everyFrameWasValid);
};

auto tNoSurfaceStillSnapshots = test("Present/aViewWithNoSurfaceStillSnapshots") = []
{
    if (!Device::shared().isValid())
        return;

    if (displayIsReachable())
        return;

    auto view = CountingView {};
    view.setBounds({0.f, 0.f, 64.f, 48.f});

    // Neither path may block when there is nowhere to present.
    view.renderNow();
    view.repaint();
    pumpUntil(Time::MS {100}, [] { return false; });

    check(view.renders == 0, "a view with no surface rendered a live frame");

    view.setContinuous(true);
    pumpUntil(Time::MS {200}, [] { return false; });

    check(view.renders == 0, "a view with no surface animated");
    view.setContinuous(false);

    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(image.width() == 64);
    check(image.height() == 48);
    check(view.renders == 1, "the snapshot did not run render() exactly once");
    check(view.everyFrameWasValid);
};
