#include "Timer.h"
#include "../ObjC/ObjC.h"
#include "ThreadUtils.h"

namespace eacp::Threads
{

struct Timer::Native
{
    Native(Callback cb, int intervalHz)
    {
        assertMainThread();
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

    ~Native()
    {
        assertMainThread();
        [nsTimer.get() invalidate];
    }

    ObjC::Ptr<NSTimer> nsTimer;
};

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
    , impl(cbToUse, intervalHz)
{
}

} // namespace eacp::Threads