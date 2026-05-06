#include "ThreadUtils-Linux.h"

#include <cassert>
#include <pthread.h>

namespace eacp::Threads
{
static pthread_t mainThreadId {};
static bool mainThreadInitialized = false;

void initMainThread()
{
    mainThreadId = pthread_self();
    mainThreadInitialized = true;
}

bool isMainThread()
{
    if (!mainThreadInitialized)
        return false;
    return pthread_equal(pthread_self(), mainThreadId) != 0;
}

void assertMainThread()
{
    assert(isMainThread() && "Must be accessed from Main Thread");
}
} // namespace eacp::Threads
