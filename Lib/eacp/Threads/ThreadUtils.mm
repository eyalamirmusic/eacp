#include "ThreadUtils.h"
#import <Foundation/Foundation.h>

namespace eacp::Threads
{
bool isMainThread()
{
    return [NSThread isMainThread];
}

void assertMainThread()
{
    assert(isMainThread() && "Must be accessed from Main Thread");
}

} // namespace eacp::Threads