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

@interface EACP_TimerProxy : NSObject
{
    eacp::Callback cb;
}
- (instancetype)initWithCallback:(eacp::Callback)cb;
- (void)timerFired:(NSTimer*)t;
@end

@implementation EACP_TimerProxy

- (instancetype)initWithCallback:(eacp::Callback)cbToUse
{
    self = [super init];
    if (self)
    {
        cb = cbToUse;
    }
    return self;
}

- (void)timerFired:(NSTimer*)t
{
    cb();
}

@end

namespace eacp::Threads
{

struct Timer::Impl
{
    Impl(const Callback& cb, int intervalHz)
    {
        AssertMainThread();

        double intervalSec = 1.0 / (double) intervalHz;

        proxy = [[EACP_TimerProxy alloc] initWithCallback:cb];

        nsTimer = [NSTimer timerWithTimeInterval:intervalSec
                                          target:proxy.get()
                                        selector:@selector(timerFired:)
                                        userInfo:nil
                                         repeats:YES];

        [[NSRunLoop mainRunLoop] addTimer:nsTimer.get()
                                  forMode:NSRunLoopCommonModes];
    }

    ~Impl()
    {
        AssertMainThread();
        [nsTimer.get() invalidate];
    }

    ObjC::Ptr<NSTimer> nsTimer;
    ObjC::Ptr<EACP_TimerProxy> proxy;
};

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
{
    impl = std::make_shared<Impl>(cbToUse, intervalHz);
}

} // namespace eacp::Threads