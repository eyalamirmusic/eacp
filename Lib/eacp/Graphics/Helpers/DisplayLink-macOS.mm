#include "DisplayLink.h"
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Threads/ThreadUtils.h>
#import <CoreVideo/CoreVideo.h>

#include <memory>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace eacp::Threads
{

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
    };

    Native(const Callback& cb)
        : state(std::make_shared<State>(cb))
    {
        assertMainThread();

        CVDisplayLinkCreateWithActiveCGDisplays(&displayLink);

        auto pending = state;
        CVDisplayLinkSetOutputHandler(
            displayLink,
            ^CVReturn(CVDisplayLinkRef,
                      const CVTimeStamp*,
                      const CVTimeStamp*,
                      CVOptionFlags,
                      CVOptionFlags*) {
              dispatch_async(dispatch_get_main_queue(), ^{
                if (pending->alive)
                    pending->callback();
              });

              return kCVReturnSuccess;
            });

        CVDisplayLinkStart(displayLink);
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
