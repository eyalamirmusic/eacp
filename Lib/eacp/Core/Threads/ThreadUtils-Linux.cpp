#include "ThreadUtils-Linux.h"

#include <cassert>
#include <pthread.h>

namespace eacp::Threads
{
// Captured at static-init time, which runs on the main thread before
// main(). Used as a fallback for isMainThread() so code that asserts
// on the main thread (e.g. AsyncPromise) works in tests that never
// pump the event loop and therefore never call initMainThread().
static const pthread_t initialMainThreadId = pthread_self();

static pthread_t mainThreadId {};
static bool mainThreadInitialized = false;

void initMainThread()
{
    mainThreadId = pthread_self();
    mainThreadInitialized = true;
}

bool isMainThread()
{
    auto self = pthread_self();
    if (mainThreadInitialized)
        return pthread_equal(self, mainThreadId) != 0;
    return pthread_equal(self, initialMainThreadId) != 0;
}

void assertMainThread()
{
    assert(isMainThread() && "Must be accessed from Main Thread");
}
} // namespace eacp::Threads
