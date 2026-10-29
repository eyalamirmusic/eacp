#include "ThreadStackProbe.h"

#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Format.h>

#include <eacp/CrashReporting/Faults/Faults.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <eacp/Core/Utils/WinInclude.h>
#include <psapi.h>
#else
#include <csignal>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#endif

namespace eacp::CrashReporting::Tests
{
namespace
{
constexpr auto threadsPerRepetition = 10000;
constexpr auto repetitions = 5;
constexpr auto threadsAliveForMemory = 100;
constexpr auto p99Fraction = 0.99;
constexpr auto bytesPerKibibyte = 1024.0;

#if defined(_WIN32)
constexpr auto stackOverflowReserveBytes = ULONG {64 * 1024};
#endif

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::duration<double, std::micro>;

double getMedian(const Vector<double>& sorted)
{
    return sorted[sorted.size() / 2];
}

double getP99(const Vector<double>& sorted)
{
    auto lastIndex = static_cast<double>(sorted.size() - 1);
    return sorted[static_cast<int>(lastIndex * p99Fraction)];
}

} // namespace

int runThreadCreationBench()
{
    auto durations = Vector<double> {};

    for (auto index = 0; index < threadsPerRepetition * repetitions; ++index)
    {
        auto start = Clock::now();
        auto thread = std::thread {[] {}};
        thread.join();
        durations.add(Microseconds {Clock::now() - start}.count());
    }

    std::ranges::sort(durations);
    Console::print(Strings::format(
        "median {:.2f} us, p99 {:.2f} us", getMedian(durations), getP99(durations)));
    return 0;
}

namespace
{
#if defined(_WIN32)
double getResidentKibibytes()
{
    auto counters = PROCESS_MEMORY_COUNTERS {};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters));
    return static_cast<double>(counters.WorkingSetSize) / bytesPerKibibyte;
}

bool hasStackOverflowStack()
{
    auto current = ULONG {0};
    ::SetThreadStackGuarantee(&current);
    return current >= stackOverflowReserveBytes;
}
#else
double getResidentKibibytes()
{
    auto info = mach_task_basic_info_data_t {};
    auto count = mach_msg_type_number_t {MACH_TASK_BASIC_INFO_COUNT};
    ::task_info(::mach_task_self(),
                MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info),
                &count);
    return static_cast<double>(info.resident_size) / bytesPerKibibyte;
}

bool hasStackOverflowStack()
{
    auto current = stack_t {};
    ::sigaltstack(nullptr, &current);
    return (current.ss_flags & SS_DISABLE) == 0;
}
#endif
} // namespace

int runThreadMemoryMeasurement()
{
    auto mutex = std::mutex {};
    auto changed = std::condition_variable {};
    auto started = 0;
    auto shouldRelease = false;

    auto before = getResidentKibibytes();
    auto threads = Vector<std::thread> {};

    for (auto index = 0; index < threadsAliveForMemory; ++index)
    {
        threads.add(std::thread {[&]
                                 {
                                     auto lock = std::unique_lock {mutex};
                                     ++started;
                                     changed.notify_all();
                                     changed.wait(lock,
                                                  [&] { return shouldRelease; });
                                 }});
    }

    {
        auto lock = std::unique_lock {mutex};
        changed.wait(lock, [&] { return started == threadsAliveForMemory; });
    }

    auto after = getResidentKibibytes();

    {
        auto lock = std::lock_guard {mutex};
        shouldRelease = true;
    }

    changed.notify_all();

    for (auto& thread: threads)
        thread.join();

    Console::print(Strings::format("{:.0f}", after - before));
    return 0;
}

int runThreadStackCheck()
{
    auto newThreadHasStack = false;
    auto thread = std::thread {[&] { newThreadHasStack = hasStackOverflowStack(); }};
    thread.join();

    Console::print("starting thread: " + std::to_string(hasStackOverflowStack()));
    Console::print("new thread: " + std::to_string(newThreadHasStack));

#if defined(__APPLE__)
    __block auto workerHasStack = false;
    auto done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      workerHasStack = hasStackOverflowStack();
      dispatch_semaphore_signal(done);
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_release(done);
    Console::print("dispatch worker: " + std::to_string(workerHasStack));
#endif
    return 0;
}

int overflowANewStdThread()
{
    auto thread = std::thread {
        [] { Faults::triggerFault("stackoverflow", Faults::ThreadChoice::main); }};
    thread.join();
    return 0;
}
} // namespace eacp::CrashReporting::Tests
