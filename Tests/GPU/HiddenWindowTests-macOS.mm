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

constexpr auto presentTimeout = Time::MS {3000};
constexpr auto settleTime = Time::MS {200};
constexpr auto hiddenObservation = Time::MS {500};
constexpr auto longestAcceptableStall = std::chrono::milliseconds {250};
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
    check(pumpUntil(presentTimeout, [&] { return !isWindowOnScreen(nativeWindow); }));

    checkStaysStoppedAndResponsive(view);

    [nativeWindow deminiaturize:nil];
    const auto rendersBefore = view.renders;
    check(pumpUntil(presentTimeout, [&] { return view.renders > rendersBefore; }),
          "rendering did not resume after the window was restored");
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

    checkStaysStoppedAndResponsive(view);

    [nativeWindow orderFrontRegardless];
    const auto rendersBefore = view.renders;
    check(pumpUntil(presentTimeout, [&] { return view.renders > rendersBefore; }),
          "rendering did not resume after the window was shown again");
};
