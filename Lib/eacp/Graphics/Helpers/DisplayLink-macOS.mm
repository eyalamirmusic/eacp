#include "DisplayLink.h"
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Threads/ThreadUtils.h>
#import <CoreVideo/CoreVideo.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace eacp::Threads
{

static CVReturn displayLinkCallback(CVDisplayLinkRef,
                                    const CVTimeStamp*,
                                    const CVTimeStamp*,
                                    CVOptionFlags,
                                    CVOptionFlags*,
                                    void* displayLinkContext)
{
    auto cb = (Callback*) displayLinkContext;

    dispatch_async(dispatch_get_main_queue(), ^{
      (*cb)();
    });

    return kCVReturnSuccess;
}

struct DisplayLink::Native
{
    Native(const Callback& cb)
        : callback(cb)
    {
        assertMainThread();

        CVDisplayLinkCreateWithActiveCGDisplays(&displayLink);
        CVDisplayLinkSetOutputCallback(displayLink, displayLinkCallback, &callback);
        CVDisplayLinkStart(displayLink);
    }

    ~Native()
    {
        assertMainThread();
        CVDisplayLinkStop(displayLink);
        CVDisplayLinkRelease(displayLink);
    }

    Callback callback;
    CVDisplayLinkRef displayLink {};
};

DisplayLink::DisplayLink(const Callback& cbToUse)
    : callback(cbToUse)
    , impl(callback)
{
}

} // namespace eacp::Threads

#pragma clang diagnostic pop
