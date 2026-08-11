#include "DisplayLink.h"
#include <eacp/Core/ObjC/ObjC.h>
#import <CoreVideo/CoreVideo.h>

#include <atomic>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace eacp::Threads
{

// Posts each vsync to the main thread. Ticks coalesce rather than piling up, so
// a handler slower than a refresh simply runs at a lower rate.
struct DisplayLink::Native
{
    // Queued ticks share ownership of this and check `alive` (main thread only)
    // rather than pointing back into a destroyed Native.
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

        // Captured by value so the handler shares ownership of the state.
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
