#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/App/Args.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Format.h>

#if defined(EACP_CRASH_REPORTING)
#include <eacp/CrashReporting/CrashReporting.h>
#endif

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

// What a stack overflow report costs a thread, and whether one is made.
//   --mode bench     median and p99 of creating and joining a std::thread
//   --mode memory    the resident growth of 100 live std::threads, in KiB
//   --mode check     whether the starting thread and a new std::thread have a
//                    stack for overflow reports
//   --mode overflow  overflows the stack of a new std::thread
// --reporting is none (start() is not called; the only choice in the build
// without crash reporting), off, startingThread or everyThread.

using namespace eacp;

namespace
{
constexpr auto threadsPerRepetition = 10000;
constexpr auto repetitions = 5;
constexpr auto threadsAliveForMemory = 100;
constexpr auto stackFrameBytes = std::size_t {4096};
volatile auto shouldKeepRecursing = true;
constexpr auto p99Fraction = 0.99;
constexpr auto bytesPerKibibyte = 1024.0;

#if defined(_WIN32)
constexpr auto stackOverflowReserveBytes = ULONG {64 * 1024};
#endif

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::duration<double, std::micro>;

#if defined(EACP_CRASH_REPORTING)
struct ReportingName
{
    std::string_view name;
    CrashReporting::StackOverflowReporting reporting;
};

constexpr auto reportingNames = std::to_array<ReportingName>({
    {"off", CrashReporting::StackOverflowReporting::off},
    {"startingThread", CrashReporting::StackOverflowReporting::startingThread},
    {"everyThread", CrashReporting::StackOverflowReporting::everyThread},
});

std::optional<CrashReporting::StackOverflowReporting>
    findReporting(std::string_view name)
{
    for (const auto& entry: reportingNames)
    {
        if (entry.name == name)
            return entry.reporting;
    }

    return std::nullopt;
}

bool startCrashReporting(std::string_view reportingName,
                         const std::string& databaseDirectory)
{
    auto reporting = findReporting(reportingName);

    if (!reporting)
        return false;

    auto options = CrashReporting::Options {};
    options.stackOverflowReporting = *reporting;
    options.hangThreshold = {};

    if (!databaseDirectory.empty())
        options.databaseDirectory = FilePath {databaseDirectory};

    return CrashReporting::start(options);
}
#endif

double getMedian(const Vector<double>& sorted)
{
    return sorted[sorted.size() / 2];
}

double getP99(const Vector<double>& sorted)
{
    auto lastIndex = static_cast<double>(sorted.size() - 1);
    return sorted[static_cast<int>(lastIndex * p99Fraction)];
}

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

int runMemoryMeasurement()
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

int runCheck()
{
    auto newThreadHasStack = false;
    auto thread = std::thread {[&] { newThreadHasStack = hasStackOverflowStack(); }};
    thread.join();

    Console::print("starting thread: " + std::to_string(hasStackOverflowStack()));
    Console::print("new thread: " + std::to_string(newThreadHasStack));

#if defined(__APPLE__)
    __block auto workerHasStack = false;
    auto done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                   ^{
                     workerHasStack = hasStackOverflowStack();
                     dispatch_semaphore_signal(done);
                   });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_release(done);
    Console::print("dispatch worker: " + std::to_string(workerHasStack));
#endif
    return 0;
}

#if defined(_MSC_VER)
#pragma optimize("", off)
#endif
int recurseForever(int depth)
{
    auto frame = std::array<volatile char, stackFrameBytes> {};
    frame[0] = static_cast<char>(depth);

    if (!shouldKeepRecursing)
        return frame[0];

    return recurseForever(depth + 1) + frame[0];
}
#if defined(_MSC_VER)
#pragma optimize("", on)
#endif

int runStackOverflow()
{
    auto thread = std::thread {[] { recurseForever(0); }};
    thread.join();
    return 0;
}
} // namespace

int main(int argc, char* argv[])
{
#if defined(EACP_CRASH_REPORTING)
    if (auto code = CrashReporting::runHandlerIfRequested(argc, argv))
        return *code;
#endif

    Apps::setCommandLineArgs(argc, argv);

    auto mode = std::string {};
    auto reporting = std::string {"none"};
    auto databaseDirectory = std::string {};
    auto cli = Args::Parser {"StackOverflowProbe"};
    cli.option("--mode", mode, "bench, memory, check or overflow");
    cli.option("--reporting", reporting, "none, off, startingThread or everyThread");
    cli.option("--database", databaseDirectory, "the crash database");

    if (auto code = cli.exitCodeFor(cli.parse()))
        return *code;

#if defined(EACP_CRASH_REPORTING)
    if (reporting != "none" && !startCrashReporting(reporting, databaseDirectory))
    {
        Console::printError("could not start crash reporting with " + reporting);
        return 1;
    }
#endif

    if (mode == "bench")
        return runThreadCreationBench();

    if (mode == "memory")
        return runMemoryMeasurement();

    if (mode == "check")
        return runCheck();

    if (mode == "overflow")
        return runStackOverflow();

    Console::printError("unknown mode " + mode);
    return 1;
}
