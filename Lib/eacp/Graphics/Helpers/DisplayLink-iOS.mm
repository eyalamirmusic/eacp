#include "DisplayLink.h"
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/Threads/ThreadUtils.h>
#import <QuartzCore/CADisplayLink.h>
#import <UIKit/UIKit.h>

using DisplayLinkCallback = eacp::Callback;

@interface DisplayLinkTarget : NSObject
{
    DisplayLinkCallback* _callback;
}
- (instancetype)initWithCallback:(DisplayLinkCallback*)callback;
- (void)displayLinkFired:(CADisplayLink*)displayLink;
@end

@implementation DisplayLinkTarget
- (instancetype)initWithCallback:(DisplayLinkCallback*)callback
{
    self = [super init];
    if (self)
    {
        _callback = callback;
    }
    return self;
}

- (void)displayLinkFired:(CADisplayLink*)displayLink
{
    if (_callback)
    {
        (*_callback)();
    }
}
@end

namespace eacp::Threads
{

struct DisplayLink::Native
{
    Native(const Callback& cb)
        : callback(cb)
    {
        assertMainThread();

        target = [[DisplayLinkTarget alloc] initWithCallback:&callback];
        displayLink = [CADisplayLink displayLinkWithTarget:target
                                                  selector:@selector(displayLinkFired:)];
        [displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }

    ~Native()
    {
        assertMainThread();
        [displayLink invalidate];
    }

    Callback callback;
    CADisplayLink* displayLink = nil;
    DisplayLinkTarget* target = nil;
};

DisplayLink::DisplayLink(const Callback& cbToUse)
    : callback(cbToUse)
    , impl(callback)
{
}

} // namespace eacp::Threads
