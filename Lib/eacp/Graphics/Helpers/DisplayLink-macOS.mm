#include "DisplayLink.h"
#include <eacp/Core/ObjC/ObjC.h>
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
        CVDisplayLinkStop(displayLink);
        CVDisplayLinkRelease(displayLink);
    }

    std::shared_ptr<State> state;
    CVDisplayLinkRef displayLink {};
};

DisplayLink::DisplayLink(const FrameCallback& cb)
    : callback(timedTick(cb))
    , impl(callback)
{
}

} // namespace eacp::Threads

#pragma clang diagnostic pop
