#include "ThreadUtils-Windows.h"
#include <atomic>

namespace eacp::Threads
{
static std::atomic<DWORD> mainThreadId {0};

void initMainThread()
{
    mainThreadId = GetCurrentThreadId();
}
DWORD getMainThreadID()
{
    return mainThreadId;
}

bool isMainThread()
{
    return GetCurrentThreadId() == mainThreadId;
}

void assertMainThread()
{
    assert(isMainThread() && "Must be accessed from Main Thread");
}

} // namespace eacp::Threads
