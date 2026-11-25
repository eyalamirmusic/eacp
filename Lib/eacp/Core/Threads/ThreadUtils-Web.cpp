#include "ThreadUtils.h"

namespace eacp::Threads
{
bool isMainThread()
{
    return true;
}

void assertMainThread() {}

void setCurrentThreadName(std::string_view) {}

void setCurrentThreadPriority(ThreadPriority) {}

bool setCurrentThreadRealtime(const RealtimeOptions&)
{
    return false;
}

void endCurrentThreadRealtime() {}
} // namespace eacp::Threads
