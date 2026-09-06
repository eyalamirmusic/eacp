#include "Timer.h"
#include "../ObjC/ObjC.h"
#include "ThreadUtils.h"

namespace eacp::Threads
{

struct Timer::Native
{
    Native(const Callback& cbToUse, double intervalSec)
        : cb(cbToUse)
    {
        assertMainThread();
        assert(intervalSec > 0 && "Timer interval must be positive");

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

    Callback cb;
    ObjC::Ptr<NSTimer> nsTimer;
};

Timer::Timer(const Callback& cbToUse, Time::MS interval)
    : callback(cbToUse)
    , impl(cbToUse, (double) interval.count / 1000.0)
{
}

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
    , impl(cbToUse, 1.0 / (double) intervalHz)
{
}

} // namespace eacp::Threads