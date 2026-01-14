#include "AutoReleasePool.h"

extern "C"
{
    void* objc_autoreleasePoolPush(void);
    void objc_autoreleasePoolPop(void* context);
}

namespace eacp::ObjC
{

AutoReleasePool::AutoReleasePool()
{
    context = objc_autoreleasePoolPush();
}

AutoReleasePool::~AutoReleasePool()
{
    objc_autoreleasePoolPop(context);
}

} // namespace eacp::ObjC