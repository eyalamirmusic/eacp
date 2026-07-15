#include "ScopedAutoReleasePool.h"

#import <Foundation/Foundation.h>

namespace eacp
{
ScopedAutoReleasePool::ScopedAutoReleasePool()
    : pool([[NSAutoreleasePool alloc] init])
{
}

ScopedAutoReleasePool::~ScopedAutoReleasePool()
{
    [(NSAutoreleasePool*) pool release];
}
} // namespace eacp
