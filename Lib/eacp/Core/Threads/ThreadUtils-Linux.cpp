#include "ThreadUtils-Linux.h"
#include "ThreadRealtime.h"
#include "../Utils/Common.h"

#include <pthread.h>
#include <sched.h>

#if defined(__ANDROID__)
#include <sys/resource.h>
#include <unistd.h>
#endif

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

void setCurrentThreadName(std::string_view name)
{
    pthread_setname_np(pthread_self(), fitThreadName(name, 15).c_str());
}

#if defined(__ANDROID__)
void setCurrentThreadPriority(ThreadPriority priority)
{
    auto niceness = 0;

    if (priority == ThreadPriority::Low)
        niceness = 10;
    else if (priority == ThreadPriority::High)
        niceness = -2;

    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), niceness);
}

bool setCurrentThreadRealtime(const RealtimeOptions&)
{
    setCurrentThreadPriority(ThreadPriority::High);
    return false;
}
#else
namespace
{
bool threadPolicyIsRealtime(int policy)
{
    policy &= ~SCHED_RESET_ON_FORK;
    return policy == SCHED_RR || policy == SCHED_FIFO;
}

// glibc keeps the creator's policy in a new thread's descriptor, so the cached
// answer and the kernel's can disagree; either one being realtime counts.
bool threadRunsRealtime()
{
    auto cachedPolicy = 0;
    auto param = sched_param {};
    pthread_getschedparam(pthread_self(), &cachedPolicy, &param);

    auto kernelPolicy = sched_getscheduler(0);

    return threadPolicyIsRealtime(cachedPolicy)
           || (kernelPolicy >= 0 && threadPolicyIsRealtime(kernelPolicy));
}
} // namespace

// Levels have no effect here, but an inherited realtime policy is dropped back
// to SCHED_OTHER, which needs no privilege.
void setCurrentThreadPriority(ThreadPriority)
{
    if (!threadRunsRealtime())
        return;

    auto param = sched_param {};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
}

// SCHED_RESET_ON_FORK, so no thread this one starts is born realtime.
bool setCurrentThreadRealtime(const RealtimeOptions&)
{
    auto param = sched_param {};
    param.sched_priority = Detail::realtimeSchedulerPriority(
        sched_get_priority_min(SCHED_RR), sched_get_priority_max(SCHED_RR));

    return pthread_setschedparam(
               pthread_self(), SCHED_RR | SCHED_RESET_ON_FORK, &param)
           == 0;
}
#endif

void endCurrentThreadRealtime() {}
} // namespace eacp::Threads
