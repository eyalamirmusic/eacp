#include "ThreadUtils.h"
#include "ThreadRealtime.h"
#import <Foundation/Foundation.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>

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

void setCurrentThreadName(std::string_view name)
{
    pthread_setname_np(fitThreadName(name, 63).c_str());
}

void setCurrentThreadPriority(ThreadPriority priority)
{
    auto qos = QOS_CLASS_DEFAULT;

    if (priority == ThreadPriority::Low)
        qos = QOS_CLASS_UTILITY;
    else if (priority == ThreadPriority::High)
        qos = QOS_CLASS_USER_INITIATED;

    pthread_set_qos_class_self_np(qos, 0);
}

namespace
{
kern_return_t threadSetTimeConstraint(const Detail::RealtimeConstraint& times)
{
    auto timebase = mach_timebase_info_data_t {};
    mach_timebase_info(&timebase);

    auto ticks = [&](double milliseconds)
    {
        return Detail::machTicksFromMilliseconds(
            milliseconds, timebase.numer, timebase.denom);
    };

    auto policy = thread_time_constraint_policy_data_t {};
    policy.period = ticks(times.periodMs);
    policy.computation = ticks(times.computationMs);
    policy.constraint = ticks(times.constraintMs);
    policy.preemptible = true;

    return thread_policy_set(pthread_mach_thread_np(pthread_self()),
                             THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy),
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

bool threadTryRealtime(const RealtimeOptions& options)
{
    auto times = Detail::realtimeConstraintFor(options);
    auto result = threadSetTimeConstraint(times);

    if (result == KERN_INVALID_ARGUMENT && times.computationMs > 50.0)
    {
        auto clamped = options;
        clamped.processingTimeMs = 50.0;
        result = threadSetTimeConstraint(Detail::realtimeConstraintFor(clamped));
    }

    return result == KERN_SUCCESS;
}
} // namespace

bool setCurrentThreadRealtime(const RealtimeOptions& options)
{
    if (threadTryRealtime(options))
        return true;

    setCurrentThreadPriority(ThreadPriority::High);
    return false;
}

void endCurrentThreadRealtime() {}

} // namespace eacp::Threads
