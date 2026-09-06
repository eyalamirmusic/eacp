#include "Common.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Time.h>

#include <cmath>

// The swapchain half of the Linux backend: a GPUView in a real Graphics::Window
// putting frames on a Wayland compositor. Everything else in this directory is
// off-screen and needs no display at all, which is why this is the one file
// here that is platform-gated.
//
// **These cases self-skip without a compositor, and that is the dangerous
// kind.** A skipped test is a pass as far as ctest is concerned, so a lane that
// lost its Weston session would report green while running none of this. The
// escape hatch is the one DevicePresenceTests uses a level down:
// EACP_REQUIRE_DISPLAY=1 says a compositor is expected here, and a case that
// cannot find one then fails rather than skipping. The lane that runs this
// under `with-weston` sets it; a headless run and a developer's machine do not.
//
// Frames are counted rather than looked at. Whether the right pixels reached
// the screen is not a thing a client can ask a compositor, and the pixel
// comparisons already live in the off-screen suite, drawn by the same
// RenderPass through the same pipelines - so what is left for this file is the
// part only a surface can answer: that frames are produced at all, that they
// are paced, that they follow a resize, and that everything survives the
// swapchain going away.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// A compositor is reachable when the process was not asked to be headless and
// something told it where the socket is. Not a connection attempt: making one
// is the window backend's job, and a Window that cannot reach the compositor
// simply never reports a surface - which every case below detects as "no frames
// were presented" and reports as a failure rather than a skip.
bool displayIsReachable()
{
    if (Apps::getAppEnvironment().headless)
        return false;

    return !getEnvValue("WAYLAND_DISPLAY").empty();
}

// True when the case should go no further. Fails first when the lane said a
// display was expected, so the skip can never be silent there.
bool noDisplay()
{
    if (displayIsReachable())
        return false;

    check(getEnvValue("EACP_REQUIRE_DISPLAY") != "1",
          "EACP_REQUIRE_DISPLAY=1 but no Wayland compositor was reachable - "
          "every case in this file would otherwise have skipped and reported "
          "a pass");

    return true;
}

bool noDeviceOrDisplay()
{
    return !Device::shared().isValid() || noDisplay();
}

// A view that counts what it drew and remembers what it drew into.
//
// The size is read off the pass rather than off the view, deliberately: a
// pass's targetWidth/targetHeight are the drawable's real pixels, which is the
// only place the swapchain's extent is observable from app code and therefore
// the only way a resize can be checked from outside the backend.
struct CountingView final : GPUView
{
    // Protected on GPUView, because on the other two backends it is a thing a
    // subclass drives its own content with rather than a thing the app calls
    // from outside. A test is that subclass.
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

// GPUTests' main() runs inside Apps::run, so the loop belongs to the test
// binary and a case pumps it rather than starting one. Answers what `done` last
// said, so a caller can tell a satisfied wait from an expired one.
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

// Long enough that a compositor repainting at 60 Hz has had dozens of chances,
// short enough that a lane where nothing is ever presented fails in seconds
// rather than sitting until ctest's own timeout.
constexpr auto presentTimeout = Time::MS {3000};

// Whether a pass's target size is what a view of `points` logical units at
// `scale` should have produced.
//
// Within a pixel rather than exactly, and deliberately: ViewSurface says the
// buffer size is the bounds times the scale "rounded to whole pixels" without
// saying which way, so an exact equality would fail on a fractional scale for
// the wrong reason - the rounding rule, not the swapchain.
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

// Built by the caller and handed here rather than made and returned: a Window
// owns the back-pointer its content view reads through getWindow(), so it is
// neither copyable nor movable, and where it lives is the case's own business
// anyway - two of them destroy one at a chosen point.
void showWith(Graphics::Window& window, Graphics::View& view)
{
    window.setContentView(view);
    window.setVisible(true);
}
} // namespace

// The whole contract in one case: a GPUView that is the content of a shown
// window is asked to render, and what it is handed is a usable frame.
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

// Continuous mode is paced by the compositor's frame callbacks here rather than
// by a display link, so the thing worth pinning is that the pacing keeps
// running: one frame leads to the next without anything asking.
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

    // update() is what a continuous view advances its animation in, and it runs
    // before the render it belongs to - so a view that rendered three times has
    // been updated at least as often.
    check(view.updates >= view.renders - 1);

    view.setContinuous(false);

    // And it stops. Pumping after the switch must add nothing, or a view that
    // was told to stop animating goes on costing a frame per refresh.
    const auto after = view.renders;
    pumpUntil(Time::MS {200}, [] { return false; });

    check(view.renders == after, "frames kept coming after setContinuous(false)");
};

// The two on-demand entry points, which are the whole of rendering for a view
// that is not animating. Each is exactly one frame: renderNow() draws on the
// spot, repaint() coalesces into one onRepaint from the loop.
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

    // Coalesced, so any number of repaints in one turn of the loop is one
    // frame - the same contract View::repaint has on every backend.
    check(view.renders == afterRenderNow + 1,
          "repaint() rendered more than one frame");
};

// A GPUView is not the root of its window here, which is the arrangement every
// real app has and the one that makes the surface a subsurface rather than the
// window's own. A resize has to travel: bounds -> the backend's pixel size ->
// the swapchain -> the size the next pass reports.
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

// A hidden view has no surface, so it has nothing to present to - and the
// compositor sends it no frame callbacks, which is what stops continuous mode
// without anything having to notice. Showing it again gives it a surface back.
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

    // One frame may still be in flight when the surface goes, so the count is
    // sampled after the loop has had a turn rather than at the call.
    pumpUntil(Time::MS {200}, [] { return false; });

    const auto whileHidden = view.renders;
    pumpUntil(Time::MS {300}, [] { return false; });

    check(view.renders == whileHidden, "a hidden view went on presenting");

    view.setVisible(true);

    check(pumpUntil(presentTimeout, [&] { return view.renders > whileHidden; }),
          "a view that was shown again never presented");
};

// The off-screen snapshot and the swapchain are independent paths into the same
// render(), and this is the one that says so: renderToImage renders into a
// texture of its own while a swapchain is up, and neither disturbs the other.
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

    // And the view is still presenting afterwards: a snapshot must not leave
    // the swapchain, the companions or the layout tracking in a state the next
    // frame cannot start from.
    const auto after = view.renders;
    view.renderNow();

    check(view.renders == after + 1, "presenting stopped after a snapshot");
    check(view.everyFrameWasValid);
};

// Tearing a window down while its swapchain is up is the destruction order that
// is easy to get wrong - the VkSwapchainKHR has to go before the wl_surface it
// was made from - and the proof that it went right is that the next window
// works.
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

    // The window and its view are gone. Let the loop turn once so anything the
    // teardown deferred has run before the next one is built.
    pumpUntil(Time::MS {100}, [] { return false; });

    auto second = CountingView {};
    auto window = Graphics::Window {windowSized(256, 192)};
    showWith(window, second);

    check(pumpUntil(presentTimeout, [&] { return second.renders > 0; }),
          "a window built after one was destroyed never presented");
    check(second.everyFrameWasValid);
};

// The one case here that does not need a compositor, and the reason it is worth
// having: it pins what a GPUView does when there is no surface at all - which
// is what every headless lane runs, and what a machine with no window-system
// integration in its driver gets even with a compositor running.
//
// Nothing is presented, nothing hangs, and the off-screen path is untouched.
auto tNoSurfaceStillSnapshots = test("Present/aViewWithNoSurfaceStillSnapshots") = []
{
    if (!Device::shared().isValid())
        return;

    if (displayIsReachable())
        return;

    auto view = CountingView {};
    view.setBounds({0.f, 0.f, 64.f, 48.f});

    // Neither of the on-demand paths has anywhere to present to, so neither
    // draws - and neither blocks, which is the half of this that a swapchain
    // waiting on an acquire would get wrong.
    view.renderNow();
    view.repaint();
    pumpUntil(Time::MS {100}, [] { return false; });

    check(view.renders == 0, "a view with no surface rendered a live frame");

    // Continuous mode is paced by frame callbacks that will never arrive, so it
    // is switched on and quietly does nothing rather than spinning.
    view.setContinuous(true);
    pumpUntil(Time::MS {200}, [] { return false; });

    check(view.renders == 0, "a view with no surface animated");
    view.setContinuous(false);

    // And the snapshot path, which is what the whole GPU suite rides on, is
    // exactly as it was before there was a swapchain to be had.
    auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(image.width() == 64);
    check(image.height() == 48);
    check(view.renders == 1, "the snapshot did not run render() exactly once");
    check(view.everyFrameWasValid);
};
