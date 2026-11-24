#import <Cocoa/Cocoa.h>

#include "Common.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/Time.h>

#include <algorithm>
#include <chrono>

// A continuous GPUView in a window that is minimised or ordered out stops
// rendering, keeps the main thread free, and resumes when the window is back.
// The window is moved with NSWindow calls from the test itself, never input.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct CountingView final : GPUView
{
    void render(Frame& frame) override
    {
        ++renders;
        auto pass = frame.beginPass({});
    }

    void update(Threads::FrameTime) override { ++updates; }

    NSWindow* getNativeWindow() { return ((__bridge NSView*) getHandle()).window; }

    int renders = 0;
    int updates = 0;
};

bool noDeviceOrDisplay()
{
    return !Device::shared().isValid() || Apps::getAppEnvironment().headless;
}

// Returns the longest single event-loop slice seen, which is how long the main
// thread was unavailable to anything else queued on it.
std::chrono::milliseconds pumpFor(Time::MS duration)
{
    constexpr auto slice = Time::MS {8};

    auto longest = std::chrono::milliseconds {0};
    auto deadline = Time::Deadline {duration};

    while (!deadline.expired())
    {
        const auto before = std::chrono::steady_clock::now();
        Threads::runEventLoopFor(slice);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - before);
        longest = std::max(longest, elapsed);
    }

    return longest;
}

struct PumpResult
{
    bool done = false;
    std::chrono::milliseconds longestSlice {0};
};

PumpResult pumpUntilMeasuring(Time::MS limit, const std::function<bool()>& done)
{
    constexpr auto slice = Time::MS {8};

    auto result = PumpResult {};
    auto deadline = Time::Deadline {limit};

    while (!deadline.expired())
    {
        if (done())
        {
            result.done = true;
            return result;
        }

        const auto before = std::chrono::steady_clock::now();
        Threads::runEventLoopFor(slice);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - before);
        result.longestSlice = std::max(result.longestSlice, elapsed);
    }

    result.done = done();
    return result;
}

bool pumpUntil(Time::MS limit, const std::function<bool()>& done)
{
    return pumpUntilMeasuring(limit, done).done;
}

constexpr auto presentTimeout = Time::MS {3000};
constexpr auto settleTime = Time::MS {200};
constexpr auto hiddenObservation = Time::MS {500};

// The window server reports a window off screen only once it has acted on
// orderOut or finished the minimise animation: about 300 ms and 550 ms on an
// idle Mac, and seconds on a loaded CI runner.
constexpr auto windowServerTimeout = Time::MS {10000};

// The stall these cases exist to catch is nextDrawable giving up, which takes a
// full second. Half of that still catches it, and leaves room for the
// scheduling hiccups of a runner with more test processes than cores.
constexpr auto nextDrawableTimeout = std::chrono::milliseconds {1000};
constexpr auto longestAcceptableStall = nextDrawableTimeout / 2;
constexpr auto windowWidth = 320;
constexpr auto windowHeight = 240;

Graphics::WindowOptions makeTestWindowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.title = "eacp hidden window tests";
    options.width = windowWidth;
    options.height = windowHeight;
    return options;
}

bool isWindowOnScreen(NSWindow* window)
{
    return (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
}

// A machine whose screen is locked or asleep reports every window occluded, so
// the visible half of these cases cannot run there.
bool startsRendering(CountingView& view)
{
    constexpr auto framesToSee = 3;

    const auto rendered = pumpUntil(presentTimeout,
                                    [&]
                                    {
                                        return view.renders >= framesToSee
                                               && isWindowOnScreen(
                                                   view.getNativeWindow());
                                    });

    if (rendered)
        return true;

    LOG("HiddenWindow: skipped, the test window never rendered on screen");
    return false;
}

bool hasLeftScreen(NSWindow* window)
{
    return pumpUntil(windowServerTimeout, [&] { return !isWindowOnScreen(window); });
}

// Fifteen frames in three seconds cannot be reached if nextDrawable is timing
// out (one second each once the three drawables are spent), and the stall
// bound catches a single timeout on its own. Measuring starts after the first
// frame, so the window server's work of bringing a new window up is not
// counted as a stall of the view.
void checkRendersContinuously(CountingView& view)
{
    constexpr auto framesToSee = 15;
    constexpr auto limit = Time::MS {3000};

    check(pumpUntil(presentTimeout, [&] { return view.renders > 0; }),
          "a continuous view that was never on screen did not render a frame");

    const auto result = pumpUntilMeasuring(limit,
                                           [&]
                                           {
                                               return view.renders >= framesToSee
                                                      && view.updates >= framesToSee;
                                           });

    if (!result.done)
        LOGF("HiddenWindow: {} renders and {} updates in {} ms",
             view.renders,
             view.updates,
             limit.count);

    check(result.done, "a continuous view that was never on screen did not render");
    check(result.longestSlice < longestAcceptableStall,
          "rendering a view that was never on screen stalled the main thread");
}

NSView* nativeViewOf(GPUView& view)
{
    return (__bridge NSView*) view.getHandle();
}

NSWindow* makeWindowNeverOrderedIn()
{
    auto* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, windowWidth, windowHeight)
                  styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.releasedWhenClosed = NO;
    return window;
}

void checkStaysStoppedAndResponsive(CountingView& view)
{
    pumpFor(settleTime);

    const auto rendersBefore = view.renders;
    const auto updatesBefore = view.updates;
    const auto longestStall = pumpFor(hiddenObservation);

    check(view.renders == rendersBefore, "frames kept rendering while hidden");
    check(view.updates == updatesBefore, "update() kept running while hidden");
    check(longestStall < longestAcceptableStall,
          "the main thread stalled while the window was hidden");
}
} // namespace

auto tMinimisedStops = test("HiddenWindow/minimisedWindowStopsAndResumes") = []
{
    if (noDeviceOrDisplay())
        return;

    auto view = CountingView {};
    auto window = Graphics::Window {makeTestWindowOptions()};
    window.setContentView(view);
    window.setVisible(true);
    view.setContinuous(true);

    if (!startsRendering(view))
        return;

    auto* nativeWindow = view.getNativeWindow();
    [nativeWindow miniaturize:nil];

    if (!hasLeftScreen(nativeWindow))
    {
        LOGF("HiddenWindow: still on screen after miniaturize, miniaturized {}",
             nativeWindow.isMiniaturized == YES);
        check(false, "the minimised window never left the screen");
        return;
    }

    checkStaysStoppedAndResponsive(view);

    [nativeWindow deminiaturize:nil];
    const auto rendersBefore = view.renders;
    check(pumpUntil(presentTimeout, [&] { return view.renders > rendersBefore; }),
          "rendering did not resume after the window was restored");
};

// The cases below run headless too: a window that never reached the screen has
// no occlusion change to come, so it must render exactly as a visible one.
auto tNeverOrderedIn =
    test("HiddenWindow/aWindowNeverOrderedInRendersContinuously") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = CountingView {};
    auto* window = makeWindowNeverOrderedIn();
    [window.contentView addSubview:nativeViewOf(view)];
    view.setBounds({0.f, 0.f, (float) windowWidth, (float) windowHeight});
    view.setContinuous(true);

    checkRendersContinuously(view);

    view.setContinuous(false);
    [nativeViewOf(view) removeFromSuperview];
    [window release];
};

auto tEacpWindow = test("HiddenWindow/aShownEacpWindowRendersContinuously") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = CountingView {};
    auto window = Graphics::Window {makeTestWindowOptions()};
    window.setContentView(view);
    window.setVisible(true);
    view.setContinuous(true);

    checkRendersContinuously(view);
};

auto tNoWindow = test("HiddenWindow/aViewWithNoWindowRendersContinuously") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = CountingView {};
    view.setBounds({0.f, 0.f, (float) windowWidth, (float) windowHeight});

    // Nothing lays out a view in no window, so it is sized by hand, as the
    // MultiView cases size theirs.
    view.resizeStarted();
    view.setContinuous(true);

    checkRendersContinuously(view);
};

auto tOrderedOutStops = test("HiddenWindow/orderedOutWindowStopsAndResumes") = []
{
    if (noDeviceOrDisplay())
        return;

    auto view = CountingView {};
    auto window = Graphics::Window {makeTestWindowOptions()};
    window.setContentView(view);
    window.setVisible(true);
    view.setContinuous(true);

    if (!startsRendering(view))
        return;

    auto* nativeWindow = view.getNativeWindow();
    [nativeWindow orderOut:nil];

    if (!hasLeftScreen(nativeWindow))
    {
        check(false, "the ordered-out window never left the screen");
        return;
    }

    checkStaysStoppedAndResponsive(view);

    [nativeWindow orderFrontRegardless];
    const auto rendersBefore = view.renders;
    check(pumpUntil(presentTimeout, [&] { return view.renders > rendersBefore; }),
          "rendering did not resume after the window was shown again");
};
