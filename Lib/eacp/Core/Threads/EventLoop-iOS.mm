#include "EventLoop.h"
#import <Foundation/Foundation.h>

namespace eacp::Threads
{
void EventLoop::run()
{
    CFRunLoopRun();
}

void EventLoop::quit()
{
    CFRunLoopStop(CFRunLoopGetMain());
}
} // namespace eacp::Threads
