#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Strings.h>
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
constexpr auto testHangThresholdMilliseconds = "500";

struct ExpectedCrash
{
    std::string_view kind;
    std::string_view reason;
    bool hasStack = true;
    std::string_view messageFragment = {};
    bool stackReachesApp = true;

    // Off for a stack overflow, whose frames are all the recursion.
    bool stackReachesThreadEntry = true;
};

enum class FaultThread
{
    main,
    background
};

std::string getFaultThreadName(FaultThread thread)
{
    if (thread == FaultThread::background)
        return "background";

    return "main";
}

// A background fault's frames run up through the zoo's thread entry.
constexpr auto backgroundEntryFunction = "runFaultOnBackgroundThread";

#if defined(_WIN32)
constexpr auto nullPointerCrash =
    ExpectedCrash {"nullptr", "EXCEPTION_ACCESS_VIOLATION"};
constexpr auto divideCrash =
    ExpectedCrash {"divide", "EXCEPTION_INT_DIVIDE_BY_ZERO"};
constexpr auto stackOverflowCrash = ExpectedCrash {
    "stackoverflow", "EXCEPTION_STACK_OVERFLOW", true, {}, true, false};
constexpr auto abortCrash = ExpectedCrash {"abort", "SIGABRT"};
constexpr auto pureVirtualCrash = ExpectedCrash {"purevirtual", "purecall"};
constexpr auto heapCorruptionCrash =
    ExpectedCrash {"heapcorruption", "STATUS_HEAP_CORRUPTION", false};
constexpr auto backgroundHeapCorruptionCrash = heapCorruptionCrash;
constexpr auto fastFailCrash =
    ExpectedCrash {"fastfail", "STATUS_STACK_BUFFER_OVERRUN", false};
#else
constexpr auto nullPointerCrash = ExpectedCrash {"nullptr", "SIGSEGV"};
constexpr auto divideCrash = ExpectedCrash {"divide", "SIGFPE"};
constexpr auto stackOverflowCrash = ExpectedCrash {"stackoverflow", "SIGSEGV"};

// A secondary thread's guard page is a protection fault, which macOS raises
// as SIGBUS where the main thread's is SIGSEGV.
constexpr auto backgroundStackOverflowCrash =
    ExpectedCrash {"stackoverflow", "SIGBUS", true, {}, true, false};
constexpr auto abortCrash = ExpectedCrash {"abort", "SIGABRT"};
constexpr auto pureVirtualCrash = ExpectedCrash {"purevirtual", "SIGABRT"};
constexpr auto heapCorruptionCrash = ExpectedCrash {"heapcorruption", "SIGTRAP"};

// The main thread's small blocks come from a fast-path allocator whose check
// traps; another thread's go through the zone allocator, which aborts.
constexpr auto backgroundHeapCorruptionCrash =
    ExpectedCrash {"heapcorruption", "SIGABRT"};
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
                                               "the crash zoo threw this on purpose",
                                               terminateReachesApp};

// Matches the zoo's own availability: x86 traps a divide by zero, and MSVC
// checks for one on ARM64.
#if defined(__x86_64__) || defined(_M_X64)                                          \
    || (defined(_M_ARM64) && !defined(__clang__))
constexpr auto integerDivideTraps = true;
#else
constexpr auto integerDivideTraps = false;
#endif

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

Processes::ProcessOptions
    makeCrashOptions(std::string_view kind,
                     FaultThread thread,
                     const FilePath& database,
                     const Vector<std::string>& appArguments = {})
{
    auto options = Processes::ProcessOptions {};
    options.executable = EACP_SELF_DESTRUCT;
    options.arguments = {"--crash",
                         std::string {kind},
                         "--thread",
                         getFaultThreadName(thread),
                         "--crash-database",
                         database.str()};

    for (const auto& argument: appArguments)
        options.arguments.add(argument);

    options.arguments.add("--");
    options.arguments.add("--no-report-window");
    return options;
}

CrashReport crashSelfDestruct(std::string_view kind, FaultThread thread)
{
    auto database = TemporaryFile {};
    auto options = makeCrashOptions(kind, thread, database.path());

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

Processes::ProcessOptions
    makeHangOptions(std::string_view kind,
                    const FilePath& database,
                    std::string_view threshold,
                    const Vector<std::string>& appArguments = {},
                    FaultThread thread = FaultThread::main)
{
    auto arguments =
        Vector<std::string> {"--hang-threshold-ms", std::string {threshold}};

    for (const auto& argument: appArguments)
        arguments.add(argument);

    return makeCrashOptions(kind, thread, database, arguments);
}

bool hasFrameInFunction(const CrashReport& report, std::string_view function)
{
    return std::ranges::any_of(
        report.crashingThread.frames,
        [function](const StackFrame& frame)
        { return frame.symbol.find(function) != std::string::npos; });
}

void checkHangReport(const CrashReport& report, std::string_view function)
{
    if (report.kind != ReportKind::hang)
        Console::printError("not a hang: " + report.reason + " " + report.message);

    check(report.kind == ReportKind::hang);
    check(report.reason == "Hang");
    check(report.message.starts_with("main thread unresponsive for "));
    check(report.hangDurationMilliseconds >= 1000);
    check(report.tags.at("project") == "eacp SelfDestruct");
    check(report.breadcrumbs.size() == 2);

    if (!hasFrameInFunction(report, function))
        printFrames(report);

#if defined(_M_ARM64) && defined(__clang__)
    // clang-cl leaves frame records out of optimised ARM64 functions, so the
    // frame-pointer walk there skips the faulting function itself.
    check(hasSelfDestructFrame(report));
#else
    check(hasFrameInFunction(report, function));
#endif
}

// The hang fault sleeps past the threshold and returns, so the app exits on
// its own and the report is already written by then.
Vector<CrashReport> runHangFault(const Vector<std::string>& appArguments,
                                 std::string_view threshold,
                                 std::string_view kind = "hang",
                                 FaultThread thread = FaultThread::main)
{
    auto database = TemporaryFile {};
    auto options =
        makeHangOptions(kind, database.path(), threshold, appArguments, thread);

    auto result = Processes::run(options);
    check(result.launched);
    check(result.exitCode == 0);

    auto reportDatabase = ReportDatabase {database.path()};
    check(!reportDatabase.hasCrashedLastRun());
    return reportDatabase.readPendingReports();
}

CrashReport checkCrashReport(const ExpectedCrash& expected,
                             FaultThread thread = FaultThread::main)
{
    auto report = crashSelfDestruct(expected.kind, thread);

    check(report.reason == expected.reason);
    check(report.tags["project"] == "eacp SelfDestruct");
    check(report.extra.contains("launchedFrom"));
    check(report.metadata["environment"] == "demo");
    check(report.metadata.contains("release"));
    check(!report.metadata["arch"].empty());
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

    auto shouldReachEntry = shouldReachApp && expected.stackReachesThreadEntry
                            && thread == FaultThread::background;

    if (shouldReachEntry && !hasFrameInFunction(report, backgroundEntryFunction))
        printFrames(report);

    if (shouldReachEntry)
        check(hasFrameInFunction(report, backgroundEntryFunction));

    return report;
}

void checkUncaughtExceptionReport(FaultThread thread)
{
    auto report = checkCrashReport(terminateCrash, thread);

    if (!terminateReachesApp)
        return;

    if (!hasFrameInFunction(report, "throwUncaughtException"))
        printFrames(report);

    check(hasFrameInFunction(report, "throwUncaughtException"));

#if defined(__APPLE__)
    // The throw site is the crashing thread; where terminate aborted is kept
    // beside it.
    check(!report.crashingThread.frames.empty()
          && report.crashingThread.frames[0].symbol.find("throwUncaughtException")
                 != std::string::npos);
    check(!report.terminateFrames.empty());
#endif
}

// A stall off the main thread leaves the main thread beating, so nothing is
// reported yet and the app quits on its own once the stall would have ended.
void checkBackgroundStallIsNotReported(std::string_view kind)
{
    check(runHangFault(
              {}, testHangThresholdMilliseconds, kind, FaultThread::background)
              .empty());
}

// Never returns, so the report is waited for while the app still runs.
CrashReport waitForStallReport(std::string_view kind)
{
    auto database = TemporaryFile {};
    auto process = Processes::Process {
        makeHangOptions(kind, database.path(), testHangThresholdMilliseconds)};
    check(process.launched());

    auto reportDatabase = ReportDatabase {database.path()};
    waitForCompleteReport(reportDatabase);
    check(process.isRunning());
    process.kill();
    process.wait();

    auto reports = reportDatabase.readPendingReports();
    check(reports.size() == 1);

    return reports.empty() ? CrashReport {} : reports[0];
}

Vector<std::string> runPendingReportsProbe(const FilePath& database)
{
    auto options = Processes::ProcessOptions {};
    options.executable = EACP_PENDING_REPORTS_PROBE;
    options.arguments = {"--database", database.str(), "--expect", "1"};
    auto result = Processes::run(options);
    check(result.exitCode == 0);

    auto delivered = Vector<std::string> {};

    for (const auto& line: Strings::split(result.output, '\n'))
    {
        constexpr auto deliveredPrefix = std::string_view {"delivered: "};
        auto trimmed = Strings::trim(line);

        if (trimmed.starts_with(deliveredPrefix))
            delivered.add(trimmed.substr(deliveredPrefix.size()));
    }

    return delivered;
}

// The handler is killed before the fault, so the app writes its own report,
// and the next launch delivers it.
void checkFallbackReport(std::string_view kind,
                         FaultThread thread,
                         std::string_view reason)
{
    auto database = TemporaryFile {};
    auto result = Processes::run(
        makeCrashOptions(kind, thread, database.path(), {"--kill-handler"}));
    check(result.launched);
    check(result.exitCode != 0);

    auto reportDatabase = ReportDatabase {database.path()};
    check(reportDatabase.hasCrashedLastRun());
    auto reports = reportDatabase.readPendingReports();
    check(reports.size() == 1);

    if (reports.size() != 1)
        return;

    const auto& report = reports[0];
    check(report.capturedBy == CaptureSource::app);
    check(report.reason == reason);
    check(report.kind == ReportKind::crash);
    check(report.tags.at("project") == "eacp SelfDestruct");
    check(report.metadata.at("environment") == "demo");
    check(report.user.id == "self-destruct-user");
    check(report.crashingThread.threadId != 0);

    if (!hasSelfDestructFrame(report))
        printFrames(report);

    check(hasSelfDestructFrame(report));

    auto delivered = runPendingReportsProbe(database.path());
    check(delivered.size() == 1);
    check(!delivered.empty() && delivered[0] == report.id);
}
} // namespace

auto tCrashReportNullPointer = test("CrashReport/nullPointer") = []
{
    auto report = checkCrashReport(nullPointerCrash);
    check(hasSelfDestructFrameWithModuleUuid(report));
};

auto tCrashReportNullPointerBackground =
    test("CrashReport/nullPointerBackground") = []
{ checkCrashReport(nullPointerCrash, FaultThread::background); };

auto tCrashReportDivideByZero = test("CrashReport/divideByZero") = []
{
    if (integerDivideTraps)
        checkCrashReport(divideCrash);
};

auto tCrashReportDivideByZeroBackground =
    test("CrashReport/divideByZeroBackground") = []
{
    if (integerDivideTraps)
        checkCrashReport(divideCrash, FaultThread::background);
};

auto tCrashReportStackOverflow =
    test("CrashReport/stackOverflow") = [] { checkCrashReport(stackOverflowCrash); };

auto tCrashReportStackOverflowBackground =
    test("CrashReport/stackOverflowBackground") = []
{
#if defined(__APPLE__)
    auto report =
        checkCrashReport(backgroundStackOverflowCrash, FaultThread::background);
    check(hasFrameInFunction(report, "recurseForever"));
#else
    checkCrashReport(stackOverflowCrash, FaultThread::background);
#endif
};

auto tCrashReportAbort =
    test("CrashReport/abort") = [] { checkCrashReport(abortCrash); };

auto tCrashReportAbortBackground = test("CrashReport/abortBackground") = []
{ checkCrashReport(abortCrash, FaultThread::background); };

auto tCrashReportPureVirtual =
    test("CrashReport/pureVirtual") = [] { checkCrashReport(pureVirtualCrash); };

auto tCrashReportPureVirtualBackground =
    test("CrashReport/pureVirtualBackground") = []
{ checkCrashReport(pureVirtualCrash, FaultThread::background); };

auto tCrashReportHeapCorruption = test("CrashReport/heapCorruption") = []
{ checkCrashReport(heapCorruptionCrash); };

auto tCrashReportHeapCorruptionBackground =
    test("CrashReport/heapCorruptionBackground") = []
{ checkCrashReport(backgroundHeapCorruptionCrash, FaultThread::background); };

auto tCrashReportUncaughtException = test("CrashReport/uncaughtException") = []
{ checkUncaughtExceptionReport(FaultThread::main); };

auto tCrashReportUncaughtExceptionBackground =
    test("CrashReport/uncaughtExceptionBackground") = []
{ checkUncaughtExceptionReport(FaultThread::background); };

#if defined(_WIN32)
auto tCrashReportFastFail =
    test("CrashReport/fastFail") = [] { checkCrashReport(fastFailCrash); };

auto tCrashReportFastFailBackground = test("CrashReport/fastFailBackground") = []
{ checkCrashReport(fastFailCrash, FaultThread::background); };
#endif

auto tCrashReportHang = test("CrashReport/hang") = []
{
    auto reports = runHangFault({}, testHangThresholdMilliseconds);

    if (reports.size() != 1)
        Console::printError(std::to_string(reports.size()) + " hang reports");

    check(reports.size() == 1);

    if (reports.size() == 1)
        checkHangReport(reports[0], "sleepPastHangThreshold");
};

auto tCrashReportHangThresholdZero = test("CrashReport/hangThresholdZero") = []
{ check(runHangFault({}, "0").empty()); };

auto tCrashReportHangPaused = test("CrashReport/hangPaused") = []
{
    check(runHangFault({"--pause-hang-detection"}, testHangThresholdMilliseconds)
              .empty());
};

auto tCrashReportSpin = test("CrashReport/spin") = []
{ checkHangReport(waitForStallReport("spin"), "spinForever"); };

auto tCrashReportDeadlock = test("CrashReport/deadlock") = []
{ checkHangReport(waitForStallReport("deadlock"), "lockInOppositeOrder"); };

auto tCrashReportHangBackground = test("CrashReport/hangBackground") = []
{ checkBackgroundStallIsNotReported("hang"); };

auto tCrashReportSpinBackground = test("CrashReport/spinBackground") = []
{ checkBackgroundStallIsNotReported("spin"); };

auto tCrashReportDeadlockBackground = test("CrashReport/deadlockBackground") = []
{ checkBackgroundStallIsNotReported("deadlock"); };

auto tCrashReportFallback = test("CrashReport/fallbackWhenHandlerIsGone") = []
{ checkFallbackReport("nullptr", FaultThread::main, nullPointerCrash.reason); };

auto tCrashReportFallbackBackground =
    test("CrashReport/fallbackWhenHandlerIsGoneBackground") = []
{ checkFallbackReport("terminate", FaultThread::background, "std::terminate"); };
