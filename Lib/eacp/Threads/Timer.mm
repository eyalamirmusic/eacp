#include "Timer.h"
#import <Cocoa/Cocoa.h>
#include "../ObjC/ObjC.h"
#include <cassert>

bool isMainThread()
{
    return [NSThread isMainThread];
}

static void AssertMainThread()
{
    assert(isMainThread() && "You must call this on the main thread");
}

namespace eacp::Threads
{
static void AssertMainThread()
{
    assert([NSThread isMainThread] && "Timer must be accessed from Main Thread");
}

struct Timer::Impl
{
    Impl(Callback cb, int intervalHz)
    {
        AssertMainThread();
        double intervalSec = 1.0 / (double) intervalHz;

        auto timerBlock = ^(NSTimer* _Nonnull) {
          cb();
        };

        nsTimer.reset([NSTimer timerWithTimeInterval:intervalSec
                                             repeats:YES
                                               block:timerBlock]);

        [[NSRunLoop mainRunLoop] addTimer:nsTimer.get()
                                  forMode:NSRunLoopCommonModes];
    }

    ~Impl()
    {
        AssertMainThread();
        [nsTimer.get() invalidate];
    }

    ObjC::Ptr<NSTimer> nsTimer;
};

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
{
    impl = std::make_shared<Impl>(cbToUse, intervalHz);
}

} // namespace eacp::Threads