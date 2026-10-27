#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/CrashReporting/ReportDatabase.h>

#include <NanoTest/NanoTest.h>

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

constexpr auto terminateCrash = ExpectedCrash {
    "terminate", "std::terminate", true, "SelfDestruct threw this on purpose"};

bool hasSelfDestructFrame(const CrashReport& report)
{
    for (const auto& frame: report.crashingThread.frames)
    {
        if (frame.modulePath.find(selfDestructModuleName) != std::string::npos)
            return true;
    }

    return false;
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

    auto reports = reportDatabase.readPendingReports();
    check(reports.size() == 1);

    return reports.empty() ? CrashReport {} : reports[0];
}

void checkCrashReport(const ExpectedCrash& expected)
{
    auto report = crashSelfDestruct(expected.kind);

    check(report.reason == expected.reason);
    check(report.metadata["project"] == "eacp SelfDestruct");
    check(report.attachments.size() == 1);

    check(report.message.find(expected.messageFragment) != std::string::npos);

    if (expected.hasStack)
        check(hasSelfDestructFrame(report));
    else
        check(report.crashingThread.frames.empty());
}
} // namespace

auto tCrashReportNullPointer =
    test("CrashReport/nullPointer") = [] { checkCrashReport(nullPointerCrash); };

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
