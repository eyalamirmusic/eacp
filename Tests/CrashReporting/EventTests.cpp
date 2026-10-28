#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/CrashReporting/ReportDatabase.h>
#include <eacp/CrashReporting/Faults/Faults.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>

using namespace nano;
using namespace eacp;
using namespace eacp::CrashReporting;

namespace EventTests
{
constexpr auto stillRunningLine = "SelfDestruct: still running after ";
constexpr auto eventReportedLine = "SelfDestruct: event ";

Processes::ProcessResult runEvent(std::string_view kind, const FilePath& database)
{
    auto options = Processes::ProcessOptions {};
    options.executable = EACP_SELF_DESTRUCT;
    options.arguments = {
        "--event", std::string {kind}, "--crash-database", database.str()};
    return Processes::run(options);
}

bool hasCallerFrame(const CrashReport& report, std::string_view caller)
{
    return std::ranges::any_of(
        report.crashingThread.frames,
        [caller](const StackFrame& frame)
        { return frame.symbol.find(caller) != std::string::npos; });
}

// The app reports the event, hands it to eventReported, and goes on to exit
// normally; nothing marks the run as crashed.
CrashReport checkEvent(std::string_view kind, std::string_view caller)
{
    auto directory = TemporaryFile {};
    auto result = runEvent(kind, directory.path());
    check(result.launched);
    check(result.exitCode == 0);

    auto database = ReportDatabase {directory.path()};
    check(!database.hasCrashedLastRun());

    auto reports = database.readPendingReports();
    check(reports.size() == 1);

    if (reports.empty())
        return {};

    const auto& report = reports[0];
    check(report.kind == ReportKind::event);
    check(report.capturedBy == CaptureSource::app);
    check(report.minidumpPath.empty());
    check(report.tags.contains("project"));
    check(report.user.id == "self-destruct-user");
    check(report.breadcrumbs.size() == 2);
    check(result.output.find(eventReportedLine + report.id) != std::string::npos);
    check(result.output.find(stillRunningLine + report.id) != std::string::npos);

    if (!hasCallerFrame(report, caller))
        Console::printError(result.output);

    check(hasCallerFrame(report, caller));
    return report;
}

auto tMessage = test("CrashEvent/messageIsReportedAndTheAppRuns") = []
{
    auto report = checkEvent("message", "reportMessageEvent");
    check(report.reason == messageEventReason);
    check(report.level == Level::warning);
    check(report.message == Faults::zooEventMessage);
};

auto tException = test("CrashEvent/caughtExceptionIsReportedAndTheAppRuns") = []
{
    auto report = checkEvent("exception", "reportCaughtException");
    check(report.reason == exceptionEventReason);
    check(report.level == Level::error);
    check(report.message
          == "std::runtime_error: " + std::string {Faults::zooEventMessage});
};

auto tPending = test("CrashEvent/reachesPendingReportFound") = []
{
    auto directory = TemporaryFile {};
    check(runEvent("message", directory.path()).exitCode == 0);

    auto reports = ReportDatabase {directory.path()}.readPendingReports();
    check(reports.size() == 1);

    auto options = Processes::ProcessOptions {};
    options.executable = EACP_PENDING_REPORTS_PROBE;
    options.arguments = {"--database", directory.path().str(), "--expect", "1"};
    auto result = Processes::run(options);
    check(result.exitCode == 0);

    if (!reports.empty())
        check(result.output.find("delivered: " + reports[0].id)
              != std::string::npos);
};
} // namespace EventTests
