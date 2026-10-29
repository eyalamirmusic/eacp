#include <eacp/Core/Platform/Platform.h>
#include "CrashingProcess.h"

#include <eacp/CrashReporting/Faults/Matrix.h>

#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/Core/App/Args.h>
#include <eacp/CrashReporting/ReportDatabase.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <thread>

using namespace nano;
using namespace eacp;
using namespace eacp::CrashReporting;

namespace
{
constexpr auto reportWaitLimit = std::chrono::seconds {10};
constexpr auto reportPollInterval = std::chrono::milliseconds {20};
constexpr auto testHangThresholdMilliseconds = "500";

using Faults::ExpectedOutcome;
using Faults::OutcomeStatus;
using FaultThread = Faults::ThreadChoice;

std::string getFaultThreadName(FaultThread thread)
{
    return std::string {Faults::getThreadChoiceName(thread)};
}

// A background fault's frames run up through the faults library's thread entry.
constexpr auto backgroundEntryFunction = "runFaultOnBackgroundThread";
constexpr auto thrownMessage = "the crash fault threw this on purpose";

ExpectedOutcome getExpectedOutcome(std::string_view fault, FaultThread thread)
{
    auto outcome =
        Faults::findExpectedOutcome(fault, thread, Faults::getCurrentLane());
    check(outcome.has_value());
    return outcome.value_or(ExpectedOutcome {});
}

bool isReportedStatus(OutcomeStatus status)
{
    return status == OutcomeStatus::reported
           || status == OutcomeStatus::reportedWithLimits;
}

bool isCrashingProcessFrame(const StackFrame& frame)
{
    return frame.modulePath.find(Tests::crashingProcessModuleName)
           != std::string::npos;
}

bool hasCrashingProcessFrame(const CrashReport& report)
{
    return std::ranges::any_of(report.crashingThread.frames, isCrashingProcessFrame);
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

bool hasCrashingProcessFrameWithModuleUuid(const CrashReport& report)
{
    return std::ranges::any_of(report.crashingThread.frames,
                               [](const StackFrame& frame)
                               {
                                   return isCrashingProcessFrame(frame)
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
    auto arguments = Vector<std::string> {"--crash",
                                          std::string {kind},
                                          "--thread",
                                          getFaultThreadName(thread),
                                          "--crash-database",
                                          database.str()};

    for (const auto& argument: appArguments)
        arguments.add(argument);

    return Tests::makeCrashingProcessOptions(arguments);
}

CrashReport crashTheCrashingProcess(std::string_view kind, FaultThread thread)
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
    check(report.tags.at("project") == Tests::crashingProcessProject);
    check(report.breadcrumbs.size() == 2);

    if (!hasFrameInFunction(report, function))
        printFrames(report);

#if defined(_M_ARM64)
    // Optimised ARM64 functions may have no frame record (clang-cl leaves them
    // out, MSVC does for a leaf below std::mutex), so the frame-pointer walk
    // there can skip the stalled function itself.
    check(hasCrashingProcessFrame(report));
#else
    check(hasFrameInFunction(report, function));
#endif
}

// The hang fault sleeps past the threshold and returns, so the app exits on
// its own and the report is already written by then.
// A stalling fault ends its app on its own; one that does not is a failure,
// not a test run that never ends.
bool waitForExit(Processes::Process& process)
{
    constexpr auto exitWaitLimit = std::chrono::seconds {30};
    auto deadline = std::chrono::steady_clock::now() + exitWaitLimit;

    while (process.isRunning() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(reportPollInterval);

    if (!process.isRunning())
        return true;

    process.kill();
    process.wait();
    return false;
}

Vector<CrashReport> runHangFault(const Vector<std::string>& appArguments,
                                 std::string_view threshold,
                                 std::string_view kind = "hang",
                                 FaultThread thread = FaultThread::main)
{
    auto database = TemporaryFile {};
    auto options =
        makeHangOptions(kind, database.path(), threshold, appArguments, thread);

    auto process = Processes::Process {options};
    check(process.launched());
    auto exited = waitForExit(process);

    if (!exited)
        Console::printError("--crashing-process --crash " + std::string {kind}
                            + " never quit");

    check(exited);
    check(process.wait() == 0);

    // The handler may still be writing a hang report the app has outlived.
    auto reportDatabase = ReportDatabase {database.path()};

    if (!Files::listDirectory(reportDatabase.getPendingDirectory()).empty())
        waitForCompleteReport(reportDatabase);

    check(!reportDatabase.hasCrashedLastRun());
    return reportDatabase.readPendingReports();
}

bool hasExpectedReason(const CrashReport& report, const ExpectedOutcome& expected)
{
    if (report.reason == expected.reason)
        return true;

    return !expected.alternativeReason.empty()
           && report.reason == expected.alternativeReason;
}

void checkReportAgainstMatrix(const CrashReport& report,
                              const ExpectedOutcome& expected)
{
    if (!hasExpectedReason(report, expected))
        Console::printError("reason: " + report.reason);

    check(hasExpectedReason(report, expected));
    check(report.capturedBy == expected.capturedBy);
    check(report.crashingThread.frames.empty() != expected.hasStack);
    check(report.minidumpPath.empty() != expected.hasMinidump);

    auto shouldReachApp = expected.hasStack && expected.stackReachesFault;

    if (shouldReachApp && !hasCrashingProcessFrame(report))
        printFrames(report);

    if (shouldReachApp)
        check(hasCrashingProcessFrame(report));
}

void checkReportMetadata(const CrashReport& report)
{
    check(report.tags.contains("project")
          && report.tags.at("project") == Tests::crashingProcessProject);
    check(report.extra.contains("launchedFrom"));
    check(report.metadata.contains("environment")
          && report.metadata.at("environment") == "test");
    check(report.metadata.contains("release"));
    check(report.metadata.contains("arch") && !report.metadata.at("arch").empty());
    check(report.user.id == "crash-test-user");
    check(report.user.email == "user@example.com");
    check(report.user.username == "crashtest");
    check(report.breadcrumbs.size() == 2);

    if (report.breadcrumbs.size() == 2)
    {
        check(report.breadcrumbs[0].message == "crash reporting started");
        check(report.breadcrumbs[0].level == Level::info);
        check(report.breadcrumbs[1].level == Level::warning);
        check(report.breadcrumbs[0].timestamp > 0);
    }
}

CrashReport checkCrashAgainstMatrix(std::string_view fault, FaultThread thread)
{
    auto expected = getExpectedOutcome(fault, thread);
    auto report = crashTheCrashingProcess(fault, thread);

    checkReportAgainstMatrix(report, expected);
    checkReportMetadata(report);
    check(report.attachments.size() == 1);

    if (report.reason == "std::terminate")
        check(report.message.find(thrownMessage) != std::string::npos);

    return report;
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

bool isStallingFault(std::string_view fault)
{
    auto found = Faults::findFault(fault);
    return found && found->get().stallsItsThread;
}

void checkStallAgainstMatrix(std::string_view fault, const ExpectedOutcome& expected)
{
    auto report = CrashReport {};

    if (fault == "hang")
    {
        auto reports = runHangFault({}, testHangThresholdMilliseconds);
        check(reports.size() == 1);

        if (reports.size() == 1)
            report = reports[0];
    }
    else
    {
        report = waitForStallReport(fault);
    }

    checkReportAgainstMatrix(report, expected);
    check(report.kind == ReportKind::hang);
    check(report.message.starts_with("main thread unresponsive for "));
    check(report.hangDurationMilliseconds >= 1000);
    checkReportMetadata(report);
}

// The CLI refuses a fault this platform cannot raise.
void checkFaultIsRefused(std::string_view fault, FaultThread thread)
{
    auto database = TemporaryFile {};
    auto result = Processes::run(makeCrashOptions(fault, thread, database.path()));
    check(result.launched);
    check(result.exitCode == Args::badArguments);
    check(ReportDatabase {database.path()}.readPendingReports().empty());
}

void checkNotReported(std::string_view fault, FaultThread thread)
{
    if (isStallingFault(fault))
    {
        check(
            runHangFault({}, testHangThresholdMilliseconds, fault, thread).empty());
        return;
    }

    auto database = TemporaryFile {};
    Processes::run(makeCrashOptions(fault, thread, database.path()));
    check(ReportDatabase {database.path()}.readPendingReports().empty());
}

void runMatrixCell(std::string_view fault, FaultThread thread)
{
    auto expected = getExpectedOutcome(fault, thread);

    switch (expected.status)
    {
        case OutcomeStatus::pendingPR:
            Console::print("skipped, pending PR "
                           + std::to_string(expected.pullRequest) + ": "
                           + std::string {expected.note});
            return;
        case OutcomeStatus::notAvailable:
            checkFaultIsRefused(fault, thread);
            return;
        case OutcomeStatus::notReported:
            checkNotReported(fault, thread);
            return;
        case OutcomeStatus::reported:
        case OutcomeStatus::reportedWithLimits:
            break;
    }

    if (isStallingFault(fault) && thread == FaultThread::main)
        checkStallAgainstMatrix(fault, expected);
    else if (!isStallingFault(fault))
        checkCrashAgainstMatrix(fault, thread);
}

struct FaultTestName
{
    std::string_view fault;
    std::string_view behaviour;
};

constexpr auto faultTestNames = Array {
    FaultTestName {"nullptr", "nullPointer"},
    FaultTestName {"divide", "integerDivideByZero"},
    FaultTestName {"stackoverflow", "stackOverflow"},
    FaultTestName {"abort", "abort"},
    FaultTestName {"purevirtual", "pureVirtualCall"},
    FaultTestName {"terminate", "uncaughtException"},
    FaultTestName {"heapcorruption", "heapCorruption"},
    FaultTestName {"heapsmash", "heapSmash"},
    FaultTestName {"hang", "hang"},
    FaultTestName {"spin", "spin"},
    FaultTestName {"deadlock", "deadlock"},
    FaultTestName {"fastfail", "fastFail"},
    FaultTestName {"illegalinstruction", "illegalInstruction"},
    FaultTestName {"privilegedinstruction", "privilegedInstruction"},
    FaultTestName {"floatingpoint", "floatingPointException"},
    FaultTestName {"executenx", "executeNonExecutableMemory"},
    FaultTestName {"guardpage", "guardPage"},
    FaultTestName {"inpageerror", "inPageError"},
    FaultTestName {"buserror", "busError"},
    FaultTestName {"guardedfd", "guardedFileDescriptor"},
    FaultTestName {"sigsys", "badSystemCall"},
};

struct OutcomeTestName
{
    OutcomeStatus status;
    std::string_view behaviour;
};

constexpr auto outcomeTestNames = Array {
    OutcomeTestName {OutcomeStatus::reported, "IsReported"},
    OutcomeTestName {OutcomeStatus::reportedWithLimits, "IsReportedWithLimits"},
    OutcomeTestName {OutcomeStatus::notReported, "IsNotReported"},
    OutcomeTestName {OutcomeStatus::notAvailable, "CannotBeRaised"},
    OutcomeTestName {OutcomeStatus::pendingPR, "IsNotReportedYet"},
};

std::string getFaultBehaviourName(std::string_view fault)
{
    for (const auto& entry: faultTestNames)
    {
        if (entry.fault == fault)
            return std::string {entry.behaviour};
    }

    return std::string {fault};
}

std::string getOutcomeBehaviourName(OutcomeStatus status)
{
    for (const auto& entry: outcomeTestNames)
    {
        if (entry.status == status)
            return std::string {entry.behaviour};
    }

    return {};
}

std::string getThreadBehaviourName(FaultThread thread)
{
    if (thread == FaultThread::main)
        return "OnMainThread";

    return "OnBackgroundThread";
}

// One case per cell of this lane, named for what it shows, e.g.
// CrashReporting/nullPointerIsReportedOnMainThread.
bool registerMatrixCases()
{
    static auto names = std::deque<std::string> {};

    for (const auto& cell: Faults::getCrashMatrix())
    {
        if (cell.lane != Faults::getCurrentLane())
            continue;

        const auto& name =
            names.emplace_back("CrashReporting/" + getFaultBehaviourName(cell.fault)
                               + getOutcomeBehaviourName(cell.outcome.status)
                               + getThreadBehaviourName(cell.thread));
        auto fault = cell.fault;
        auto thread = cell.thread;
        test(name, [fault, thread] { runMatrixCell(fault, thread); });
    }

    return true;
}

[[maybe_unused]] const auto matrixCasesRegistered = registerMatrixCases();

int countUnreportedExits(const FilePath& database)
{
    auto options = Processes::ProcessOptions {};
    options.executable = EACP_PENDING_REPORTS_PROBE;
    options.arguments = {"--database", database.str()};
    auto result = Processes::run(options);
    check(result.exitCode == 0);

    auto count = 0;

    for (const auto& line: Strings::split(result.output, '\n'))
    {
        if (Strings::trim(line).starts_with("unreported exit: "))
            ++count;
    }

    return count;
}

// The handler is killed before the fault, so nothing reports the crash, and
// the next launch finds its session marker.
void checkUnreportedCrash(std::string_view kind, FaultThread thread)
{
    auto database = TemporaryFile {};
    auto result = Processes::run(
        makeCrashOptions(kind, thread, database.path(), {"--kill-handler"}));
    check(result.launched);
    check(result.exitCode != 0);

    auto reportDatabase = ReportDatabase {database.path()};
    check(reportDatabase.readPendingReports().empty());
    check(countUnreportedExits(database.path()) == 1);
}
} // namespace

auto tCrashReportModuleUuid = test("CrashReport/framesCarryModuleUuids") = []
{
    check(hasCrashingProcessFrameWithModuleUuid(
        crashTheCrashingProcess("nullptr", FaultThread::main)));
};

auto tCrashReportThreadEntry =
    test("CrashReport/backgroundFramesReachTheThreadEntry") = []
{
    auto report = crashTheCrashingProcess("nullptr", FaultThread::background);

    if (!hasFrameInFunction(report, backgroundEntryFunction))
        printFrames(report);

    check(hasFrameInFunction(report, backgroundEntryFunction));
};

auto tCrashReportBackgroundStackOverflow =
    test("CrashReport/backgroundStackOverflowFramesAreTheRecursion") = []
{
    check(hasFrameInFunction(
        crashTheCrashingProcess("stackoverflow", FaultThread::background),
        "recurseForever"));
};

auto tCrashReportThrowSite =
    test("CrashReport/uncaughtExceptionShowsTheThrowSite") = []
{
    auto expected = getExpectedOutcome("terminate", FaultThread::background);

    if (!expected.stackReachesFault)
        return;

    for (auto thread: {FaultThread::main, FaultThread::background})
    {
        auto report = crashTheCrashingProcess("terminate", thread);

        if (!hasFrameInFunction(report, "throwUncaughtException"))
            printFrames(report);

        check(hasFrameInFunction(report, "throwUncaughtException"));

        if (!Platform::isMac())
            continue;

        // The throw site is the crashing thread, where terminate aborted beside it.
        check(
            !report.crashingThread.frames.empty()
            && report.crashingThread.frames[0].symbol.find("throwUncaughtException")
                   != std::string::npos);
        check(!report.terminateFrames.empty());
    }
};

auto tCrashReportHangThresholdZero = test("CrashReport/hangThresholdZero") = []
{ check(runHangFault({}, "0").empty()); };

auto tCrashReportHangPaused = test("CrashReport/hangPaused") = []
{
    check(runHangFault({"--pause-hang-detection"}, testHangThresholdMilliseconds)
              .empty());
};

auto tMatrixCoversEveryFault = test("CrashMatrix/coversEveryFaultOnEveryLane") = []
{
    for (const auto& fault: Faults::listFaults())
    {
        for (auto lane: Faults::listLanes())
        {
            for (auto thread: {FaultThread::main, FaultThread::background})
                check(Faults::findExpectedOutcome(fault.name, thread, lane)
                          .has_value());
        }
    }

    for (const auto& cell: Faults::getCrashMatrix())
        check(Faults::findFault(cell.fault).has_value());
};

auto tMatrixAvailabilityAgrees =
    test("CrashMatrix/availabilityMatchesTheFaultList") = []
{
    for (const auto& fault: Faults::listFaults())
    {
        auto expected = getExpectedOutcome(fault.name, FaultThread::main);
        check((expected.status == OutcomeStatus::notAvailable) != fault.isAvailable);
    }
};

auto tCrashReportUnreportedWhenHandlerIsGone =
    test("CrashReport/unreportedExitWhenHandlerIsGone") = []
{ checkUnreportedCrash("nullptr", FaultThread::main); };

auto tCrashReportUnreportedWhenHandlerIsGoneBackground =
    test("CrashReport/unreportedExitWhenHandlerIsGoneBackground") = []
{ checkUnreportedCrash("terminate", FaultThread::background); };

auto tCrashReportReportedCrashIsNoUnreportedExit =
    test("CrashReport/reportedCrashIsNoUnreportedExit") = []
{
    auto database = TemporaryFile {};
    auto result = Processes::run(
        makeCrashOptions("nullptr", FaultThread::main, database.path()));
    check(result.launched);

    auto reportDatabase = ReportDatabase {database.path()};
    waitForCompleteReport(reportDatabase);
    check(reportDatabase.readPendingReports().size() == 1);
    check(countUnreportedExits(database.path()) == 0);
};
