#pragma once

#include <eacp/Core/Threads/ThreadRealtime.h>
#include <eacp/Core/Utils/Strings.h>

#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <eacp/Core/Utils/WinInclude.h>
#else
#include <pthread.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <sys/qos.h>
#elif defined(__linux__)
#include <sched.h>
#endif

inline std::string currentThreadNameForTest()
{
#if defined(_WIN32)
    auto* description = PWSTR {};

    if (FAILED(GetThreadDescription(GetCurrentThread(), &description)))
        return {};

    auto name = eacp::Strings::narrow(description);
    LocalFree(description);
    return name;
#else
    char buffer[64] = {};
    pthread_getname_np(pthread_self(), buffer, sizeof(buffer));
    return buffer;
#endif
}

// qos_class_self() on Apple, -1 elsewhere.
inline int currentQosForTest()
{
#if defined(__APPLE__)
    return static_cast<int>(qos_class_self());
#else
    return -1;
#endif
}

// What the calling thread's platform reports about realtime scheduling: the
// time-constraint policy in mach ticks on Apple, SCHED_RR on Linux, and the
// thread priority level on Windows, where MMCSS has no query.
struct RealtimeProbe
{
    bool scheduledRealtime = false;
    std::uint32_t period = 0;
    std::uint32_t computation = 0;
    std::uint32_t constraint = 0;
    int priorityLevel = 0;
    int schedulerPriority = 0;
};

inline RealtimeProbe probeCurrentThreadRealtime()
{
    auto probe = RealtimeProbe {};

#if defined(__APPLE__)
    auto policy = thread_time_constraint_policy_data_t {};
    auto count = mach_msg_type_number_t {THREAD_TIME_CONSTRAINT_POLICY_COUNT};
    auto getDefault = boolean_t {false};
    auto result = thread_policy_get(pthread_mach_thread_np(pthread_self()),
                                    THREAD_TIME_CONSTRAINT_POLICY,
                                    reinterpret_cast<thread_policy_t>(&policy),
                                    &count,
                                    &getDefault);

    probe.scheduledRealtime = result == KERN_SUCCESS && !getDefault;
    probe.period = policy.period;
    probe.computation = policy.computation;
    probe.constraint = policy.constraint;
#elif defined(_WIN32)
    probe.priorityLevel = GetThreadPriority(GetCurrentThread());
#elif defined(__linux__)
    auto param = sched_param {};
    sched_getparam(0, &param);
    probe.scheduledRealtime =
        (sched_getscheduler(0) & ~SCHED_RESET_ON_FORK) == SCHED_RR;
    probe.schedulerPriority = param.sched_priority;
#endif

    return probe;
}

inline std::uint32_t machTicksForTest(double milliseconds)
{
#if defined(__APPLE__)
    auto timebase = mach_timebase_info_data_t {};
    mach_timebase_info(&timebase);
    return eacp::Threads::Detail::machTicksFromMilliseconds(
        milliseconds, timebase.numer, timebase.denom);
#else
    return eacp::Threads::Detail::machTicksFromMilliseconds(milliseconds, 1, 1);
#endif
}
