#include <eacp/Core/Core.h>
#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <eacp/Core/Utils/WinInclude.h>
#include <psapi.h>
#else
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace nano;
using namespace eacp;

namespace
{
constexpr auto threadsPerRepetition = 10000;
constexpr auto repetitions = 5;
constexpr auto threadsAliveForMemory = 100;
constexpr auto handlerRanExitCode = 42;
constexpr auto overflowFrameBytes = 4096;
constexpr auto handlerRanMarker = std::string_view {"handler-ran"};
constexpr auto p99Fraction = 0.99;
constexpr auto bytesPerKibibyte = 1024.0;

constexpr auto modeFlag = std::string_view {"--mode"};
constexpr auto benchMode = std::string_view {"bench"};
constexpr auto overflowMode = std::string_view {"overflow"};
constexpr auto memoryMode = std::string_view {"memory"};
constexpr auto withFlag = std::string_view {"--with-altstack"};
constexpr auto withoutFlag = std::string_view {"--without-altstack"};

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::duration<double, std::micro>;

struct Distribution
{
    double medianMicroseconds = 0.0;
    double p99Microseconds = 0.0;
};

Distribution summariseDurations(Vector<double> durations)
{
    std::ranges::sort(durations);
    auto count = durations.size();
    auto p99Index = static_cast<int>(static_cast<double>(count - 1) * p99Fraction);
    return {durations[count / 2], durations[p99Index]};
}

template <typename StartAndJoin>
void timeEachThread(Vector<double>& durations, StartAndJoin startAndJoin)
{
    for (auto index = 0; index < threadsPerRepetition; ++index)
    {
        auto start = Clock::now();
        startAndJoin();
        durations.add(Microseconds {Clock::now() - start}.count());
    }
}

void startAndJoinEacpThread()
{
    auto thread = Threads::Thread {[] {}};
    thread.join();
}

void startAndJoinStdThread()
{
    auto thread = std::thread {[] {}};
    thread.join();
}

void printDistribution(std::string_view name, const Vector<double>& durations)
{
    auto summary = summariseDurations(durations);
    Console::print(Strings::format("{:<36} median {:8.2f} us   p99 {:8.2f} us",
                                   name,
                                   summary.medianMicroseconds,
                                   summary.p99Microseconds));
}

int runThreadCreationBench()
{
    auto withStack = Vector<double> {};
    auto withoutStack = Vector<double> {};
    auto stdThread = Vector<double> {};

    for (auto repetition = 0; repetition < repetitions; ++repetition)
    {
        Threads::setThreadsInstallAlternateSignalStack(true);
        timeEachThread(withStack, startAndJoinEacpThread);
        Threads::setThreadsInstallAlternateSignalStack(false);
        timeEachThread(withoutStack, startAndJoinEacpThread);
        timeEachThread(stdThread, startAndJoinStdThread);
    }

    Threads::setThreadsInstallAlternateSignalStack(true);
    Console::print(Strings::format("{} threads x {} repetitions, create + join",
                                   threadsPerRepetition,
                                   repetitions));
    printDistribution("Threads::Thread, alternate stack", withStack);
    printDistribution("Threads::Thread, no alternate stack", withoutStack);
    printDistribution("std::thread", stdThread);
    return 0;
}

#if defined(_WIN32)
double getResidentKibibytes()
{
    auto counters = PROCESS_MEMORY_COUNTERS {};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters));
    return static_cast<double>(counters.WorkingSetSize) / bytesPerKibibyte;
}
#elif defined(__linux__)
double getResidentKibibytes()
{
    auto statm = Files::readFile(FilePath {"/proc/self/statm"});
    auto fields = Strings::split(statm, ' ');
    auto residentPages = std::stod(fields[1]);
    return residentPages * static_cast<double>(::sysconf(_SC_PAGESIZE))
           / bytesPerKibibyte;
}
#else
// Peak rather than current, but a fresh child only grows while it measures.
double getResidentKibibytes()
{
    auto usage = rusage {};
    ::getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_maxrss) / bytesPerKibibyte;
}
#endif

int runMemoryMeasurement()
{
    auto mutex = std::mutex {};
    auto released = std::condition_variable {};
    auto started = 0;
    auto shouldRelease = false;

    auto before = getResidentKibibytes();
    auto threads = Vector<Threads::Thread> {};

    for (auto index = 0; index < threadsAliveForMemory; ++index)
    {
        threads.add(Threads::Thread {[&]
                                     {
                                         auto lock = std::unique_lock {mutex};
                                         ++started;
                                         released.notify_all();
                                         released.wait(
                                             lock, [&] { return shouldRelease; });
                                     }});
    }

    {
        auto lock = std::unique_lock {mutex};
        released.wait(lock, [&] { return started == threadsAliveForMemory; });
    }

    auto after = getResidentKibibytes();

    {
        auto lock = std::lock_guard {mutex};
        shouldRelease = true;
    }

    released.notify_all();
    threads.clear();

    Console::print(Strings::format("{:.0f}", after - before));
    return 0;
}

#if defined(_WIN32)
LONG CALLBACK reportStackOverflow(EXCEPTION_POINTERS* exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;

    auto written = DWORD {0};
    ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE),
                handlerRanMarker.data(),
                static_cast<DWORD>(handlerRanMarker.size()),
                &written,
                nullptr);
    ::TerminateProcess(::GetCurrentProcess(), handlerRanExitCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

void installOverflowReporter()
{
    ::AddVectoredExceptionHandler(1, &reportStackOverflow);
}
#else
void reportStackOverflow(int)
{
    ::write(STDOUT_FILENO, handlerRanMarker.data(), handlerRanMarker.size());
    ::_exit(handlerRanExitCode);
}

void installOverflowReporter()
{
    struct sigaction action {};
    action.sa_handler = &reportStackOverflow;
    action.sa_flags = SA_ONSTACK;
    ::sigaction(SIGSEGV, &action, nullptr);
    ::sigaction(SIGBUS, &action, nullptr);
}
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#pragma optimize("", off)
#endif
int overflowTheStack(int depth)
{
    volatile char frame[overflowFrameBytes];
    frame[0] = static_cast<char>(depth);
    return overflowTheStack(depth + 1) + frame[0];
}
#if defined(_MSC_VER) && !defined(__clang__)
#pragma optimize("", on)
#endif

int runStackOverflow(bool withAlternateStack)
{
    installOverflowReporter();
    Threads::setThreadsInstallAlternateSignalStack(withAlternateStack);
    auto thread = Threads::Thread {[] { overflowTheStack(0); }};
    thread.join();
    return 0;
}

bool hasArgument(int argc, char* argv[], std::string_view wanted)
{
    return std::ranges::any_of(Span {argv, argc},
                               [&](const char* argument)
                               { return std::string_view {argument} == wanted; });
}

std::string_view getMode(int argc, char* argv[])
{
    for (auto index = 1; index + 1 < argc; ++index)
    {
        if (std::string_view {argv[index]} == modeFlag)
            return argv[index + 1];
    }

    return {};
}

Processes::ProcessResult runSelf(std::string_view mode, std::string_view stackFlag)
{
    return Processes::run(
        Files::executablePath().nativeString(),
        {std::string {modeFlag}, std::string {mode}, std::string {stackFlag}});
}

bool hasHandlerRun(const Processes::ProcessResult& result)
{
    return result.output.find(handlerRanMarker) != std::string::npos;
}

bool isBenchRequested = false;

auto tHandlerRunsWithAlternateStack =
    test("AlternateSignalStack/overflowHandlerRunsWithTheStack") = []
{
    auto result = runSelf(overflowMode, withFlag);
    check(result.exitCode == handlerRanExitCode);
    check(hasHandlerRun(result));
};

auto tHandlerCannotRunWithoutAlternateStack =
    test("AlternateSignalStack/overflowHandlerCannotRunWithoutTheStack") = []
{
    auto result = runSelf(overflowMode, withoutFlag);
    Console::print(Strings::format("without the stack: exit code {}, output '{}'",
                                   result.exitCode,
                                   result.output));

    if (Platform::isPosix())
    {
        check(result.exitCode != handlerRanExitCode);
        check(!hasHandlerRun(result));
    }
};

auto tBench = test("AlternateSignalStack/bench") = []
{
    if (!isBenchRequested)
        return;

    auto bench = runSelf(benchMode, withFlag);
    check(bench.exitCode == 0);
    Console::print(bench.output);

    auto memoryWith = runSelf(memoryMode, withFlag);
    auto memoryWithout = runSelf(memoryMode, withoutFlag);
    Console::print(Strings::format(
        "resident delta, {} threads alive: with {} KiB, without {} KiB",
        threadsAliveForMemory,
        Strings::trim(memoryWith.output),
        Strings::trim(memoryWithout.output)));
};
} // namespace

int main(int argc, char* argv[])
{
    auto mode = getMode(argc, argv);
    auto withAlternateStack = !hasArgument(argc, argv, withoutFlag);

    if (mode == benchMode)
        return runThreadCreationBench();

    if (mode == memoryMode)
    {
        Threads::setThreadsInstallAlternateSignalStack(withAlternateStack);
        return runMemoryMeasurement();
    }

    if (mode == overflowMode)
        return runStackOverflow(withAlternateStack);

    isBenchRequested = getEnvValue("EACP_RUN_BENCH") == "1";
    return nano::run(argc, argv);
}
