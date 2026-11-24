#include "DisplayLink.h"
#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Threads/Timer.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>

#include <atomic>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace eacp::Threads
{

// Posts each vsync to the main thread. Ticks coalesce: while one is still
// queued behind a busy main thread, further vsyncs are skipped rather than
// piling up. Without that, a callback that takes longer than a refresh leaves a
// tick behind every time it runs, and since the link keeps firing on its own
// thread regardless, the queue grows without bound — the main thread ends up
// working through an ever longer run of stale ticks, never idle long enough to
// catch back up. Dropping instead lets a handler that cannot keep up simply run
// at a lower rate.
struct DisplayLink::Native
{
    // Ticks dispatched to the main queue can still be pending when the link is
    // destroyed; they share ownership of this state and check `alive` (touched
    // on the main thread only) before invoking, instead of pointing back into
    // the destroyed Native.
    struct State
    {
        explicit State(const Callback& cb)
            : callback(cb)
        {
        }

        Callback callback;
        bool alive = true;

        // Set on the link's thread as a tick is handed over, cleared on the
        // main thread as that tick starts running.
        std::atomic<bool> pending {false};
    };

    Native(const Callback& cb)
        : state(std::make_shared<State>(cb))
    {
        assertMainThread();

        if (hasNoDisplayToFollow())
        {
            startFallbackTimer();
            return;
        }

        CVDisplayLinkCreateWithActiveCGDisplays(&displayLink);

        // Captured by value, so the handler shares ownership of the state rather
        // than reaching back through a `this` that may already be gone.
        auto pending = state;

        CVDisplayLinkSetOutputHandler(
            displayLink,
            ^CVReturn(CVDisplayLinkRef,
                      const CVTimeStamp*,
                      const CVTimeStamp*,
                      CVOptionFlags,
                      CVOptionFlags*) {
              postTick(pending);
              return kCVReturnSuccess;
            });

        CVDisplayLinkStart(displayLink);
    }

    // A CVDisplayLink over no active display never fires: under EACP_HEADLESS,
    // with every display asleep, or in a session with no window server. Frames
    // still have to be driven there, so a timer at a nominal refresh stands in.
    static bool hasNoDisplayToFollow()
    {
        if (Apps::getAppEnvironment().headless)
            return true;

        auto activeDisplays = std::uint32_t {0};
        CGGetActiveDisplayList(0, nullptr, &activeDisplays);
        return activeDisplays == 0;
    }

    void startFallbackTimer()
    {
        constexpr auto nominalRefreshHz = 60;

        auto ticking = state;
        auto tick = [ticking]
        {
            if (ticking->alive)
                ticking->callback();
        };

        fallbackTimer = makeOwned<Timer>(tick, nominalRefreshHz);
    }

    static void postTick(std::shared_ptr<State> state)
    {
        if (state->pending.exchange(true))
            return;

        dispatch_async(dispatch_get_main_queue(), ^{
          state->pending = false;

          if (state->alive)
              state->callback();
        });
    }

    ~Native()
    {
        assertMainThread();
        state->alive = false;
        fallbackTimer = nullptr;

        if (displayLink == nullptr)
            return;

        CVDisplayLinkStop(displayLink);
        CVDisplayLinkRelease(displayLink);
    }

    std::shared_ptr<State> state;
    CVDisplayLinkRef displayLink {};
    OwningPointer<Timer> fallbackTimer;
};

DisplayLink::DisplayLink(const FrameCallback& cb)
    : rateLimit(std::make_shared<RateLimit>())
    , callback(rateLimited(rateLimit, timedTick(cb)))
    , impl(callback)
{
}

} // namespace eacp::Threads

#pragma clang diagnostic pop
