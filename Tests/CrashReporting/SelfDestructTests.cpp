#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/CrashReporting/ReportDatabase.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

using namespace nano;
using namespace eacp;
using namespace eacp::CrashReporting;

namespace
{
constexpr auto selfDestructModuleName = std::string_view {"SelfDestruct"};
constexpr auto reportWaitLimit = std::chrono::seconds {10};
constexpr auto reportPollInterval = std::chrono::milliseconds {20};

struct ExpectedCrash
{
    std::string_view kind;
    std::string_view reason;
    bool hasStack = true;
    std::string_view messageFragment = {};
    bool stackReachesApp = true;
};

#if defined(_WIN32)
constexpr auto nullPointerCrash =
    ExpectedCrash {"nullptr", "EXCEPTION_ACCESS_VIOLATION"};
constexpr auto stackOverflowCrash =
    ExpectedCrash {"stackoverflow", "EXCEPTION_STACK_OVERFLOW"};
constexpr auto abortCrash = ExpectedCrash {"abort", "SIGABRT"};
constexpr auto pureVirtualCrash = ExpectedCrash {"purevirtual", "purecall"};
constexpr auto heapCorruptionCrash =
    ExpectedCrash {"heapcorruption", "STATUS_HEAP_CORRUPTION", false};
constexpr auto fastFailCrash =
    ExpectedCrash {"fastfail", "STATUS_STACK_BUFFER_OVERRUN", false};
#else
constexpr auto nullPointerCrash = ExpectedCrash {"nullptr", "SIGSEGV"};
constexpr auto stackOverflowCrash = ExpectedCrash {"stackoverflow", "SIGSEGV"};
constexpr auto abortCrash = ExpectedCrash {"abort", "SIGABRT"};
constexpr auto pureVirtualCrash = ExpectedCrash {"purevirtual", "SIGABRT"};
constexpr auto heapCorruptionCrash = ExpectedCrash {"heapcorruption", "SIGTRAP"};
#endif

// On Windows ARM64 the handler's walk of an uncaught C++ exception stops at
// KERNELBASE's RaiseException (see README.md, the v1 limits).
#if defined(_M_ARM64)
constexpr auto terminateReachesApp = false;
#else
constexpr auto terminateReachesApp = true;
#endif

constexpr auto terminateCrash = ExpectedCrash {"terminate",
                                               "std::terminate",
                                               true,
                                               "SelfDestruct threw this on purpose",
                                               terminateReachesApp};

bool isSelfDestructFrame(const StackFrame& frame)
{
    return frame.modulePath.find(selfDestructModuleName) != std::string::npos;
}

bool hasSelfDestructFrame(const CrashReport& report)
{
    return std::ranges::any_of(report.crashingThread.frames, isSelfDestructFrame);
}

void printFrames(const CrashReport& report)
{
    Console::printError("frames of " + report.reason + ":");

    for (const auto& frame: report.crashingThread.frames)
        Console::printError("  " + frame.modulePath + " " + frame.symbol);
}

bool isHexDigit(char character, bool upperCase)
{
    if (character >= '0' && character <= '9')
        return true;

    if (upperCase)
        return character >= 'A' && character <= 'F';

    return character >= 'a' && character <= 'f';
}

// 8-4-4-4-12 hex digits, upper case on macOS (LC_UUID) and lower case with a
// trailing -<age hex> on Windows (PDB GUID and age).
bool isModuleUuidForm(std::string_view text)
{
#if defined(_WIN32)
    constexpr auto upperCase = false;
#else
    constexpr auto upperCase = true;
#endif
    constexpr auto groupLengths = std::to_array<std::size_t>({8, 4, 4, 4, 12});
    auto position = std::size_t {0};

    for (auto groupLength: groupLengths)
    {
        if (position > 0)
        {
            if (position >= text.size() || text[position] != '-')
                return false;
            ++position;
        }

        for (auto index = std::size_t {0}; index < groupLength; ++index, ++position)
        {
            if (position >= text.size() || !isHexDigit(text[position], upperCase))
                return false;
        }
    }

#if defined(_WIN32)
    if (position + 1 >= text.size() || text[position] != '-')
        return false;

    for (++position; position < text.size(); ++position)
    {
        if (!isHexDigit(text[position], upperCase))
            return false;
    }

    return true;
#else
    return position == text.size();
#endif
}

bool hasSelfDestructFrameWithModuleUuid(const CrashReport& report)
{
    return std::ranges::any_of(report.crashingThread.frames,
                               [](const StackFrame& frame)
                               {
                                   return isSelfDestructFrame(frame)
                                          && isModuleUuidForm(frame.moduleUuid);
                               });
}

bool hasCompleteReport(const ReportDatabase& database)
{
    for (const auto& entry: Files::listDirectory(database.getPendingDirectory()))
    {
        if (Files::exists(entry.path / "report.json"))
            return true;
    }

    return false;
}

// A crash only its exit code shows is written after the app is gone, so the
// report can land a moment after Processes::run returns.
void waitForCompleteReport(const ReportDatabase& database)
{
    auto deadline = std::chrono::steady_clock::now() + reportWaitLimit;

    while (!hasCompleteReport(database)
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(reportPollInterval);
}

CrashReport crashSelfDestruct(std::string_view kind)
{
    auto database = TemporaryFile {};
    auto options = Processes::ProcessOptions {};
    options.executable = EACP_SELF_DESTRUCT;
    options.arguments = {"--crash",
                         std::string {kind},
                         "--crash-database",
                         database.path().str(),
                         "--",
                         "--no-report-window"};

    auto result = Processes::run(options);
    check(result.launched);
    check(result.exitCode != 0);

    auto reportDatabase = ReportDatabase {database.path()};
    waitForCompleteReport(reportDatabase);

    check(reportDatabase.hasCrashedLastRun());
    auto reports = reportDatabase.readPendingReports();
    check(reports.size() == 1);

    return reports.empty() ? CrashReport {} : reports[0];
}

CrashReport checkCrashReport(const ExpectedCrash& expected)
{
    auto report = crashSelfDestruct(expected.kind);

    check(report.reason == expected.reason);
    check(report.tags["project"] == "eacp SelfDestruct");
    check(report.extra.contains("launchedFrom"));
    check(report.metadata["environment"] == "demo");
    check(report.metadata.contains("release"));
    check(report.user.id == "self-destruct-user");
    check(report.user.email == "user@example.com");
    check(report.user.username == "selfdestruct");
    check(report.breadcrumbs.size() == 2);

    if (report.breadcrumbs.size() == 2)
    {
        check(report.breadcrumbs[0].message == "crash reporting started");
        check(report.breadcrumbs[0].level == Level::info);
        check(report.breadcrumbs[1].level == Level::warning);
        check(report.breadcrumbs[0].timestamp > 0);
    }
    check(report.attachments.size() == 1);

    check(report.message.find(expected.messageFragment) != std::string::npos);

    auto shouldReachApp = expected.hasStack && expected.stackReachesApp;

    if (shouldReachApp && !hasSelfDestructFrame(report))
        printFrames(report);

    if (shouldReachApp)
        check(hasSelfDestructFrame(report));
    else if (expected.hasStack)
        check(!report.crashingThread.frames.empty());
    else
        check(report.crashingThread.frames.empty());

    return report;
}
} // namespace

auto tCrashReportNullPointer = test("CrashReport/nullPointer") = []
{
    auto report = checkCrashReport(nullPointerCrash);
    check(hasSelfDestructFrameWithModuleUuid(report));
};

auto tCrashReportStackOverflow =
    test("CrashReport/stackOverflow") = [] { checkCrashReport(stackOverflowCrash); };

auto tCrashReportAbort =
    test("CrashReport/abort") = [] { checkCrashReport(abortCrash); };

auto tCrashReportPureVirtual =
    test("CrashReport/pureVirtual") = [] { checkCrashReport(pureVirtualCrash); };

auto tCrashReportHeapCorruption = test("CrashReport/heapCorruption") = []
{ checkCrashReport(heapCorruptionCrash); };

auto tCrashReportUncaughtException =
    test("CrashReport/uncaughtException") = [] { checkCrashReport(terminateCrash); };

#if defined(_WIN32)
auto tCrashReportFastFail =
    test("CrashReport/fastFail") = [] { checkCrashReport(fastFailCrash); };
#endif
