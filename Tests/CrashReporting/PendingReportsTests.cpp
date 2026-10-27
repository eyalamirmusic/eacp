#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/CrashReporting/ReportDatabase.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::CrashReporting;

namespace
{
constexpr auto pendingReportCount = 2;

std::string writePendingReport(const ReportDatabase& database)
{
    auto report = CrashReport {};
    report.id = createReportId();
    report.reason = "SIGSEGV";
    report.timestamp = getMillisecondsSinceEpoch();
    report.platform = getPlatformName();
    database.createReportFolder(report.id);
    database.writeReport(report);

    return report.id;
}

// Windows' text-mode stdout ends each line with \r\n.
std::string removeCarriageReturns(std::string text)
{
    std::erase(text, '\r');
    return text;
}

int countLines(const std::string& rawOutput, const std::string& line)
{
    auto output = removeCarriageReturns(rawOutput);
    auto count = 0;
    auto needle = line + "\n";

    for (auto at = output.find(needle); at != std::string::npos;
         at = output.find(needle, at + needle.size()))
        ++count;

    return count;
}
} // namespace

Processes::ProcessResult runProbe(const FilePath& database,
                                  Vector<std::string> extraArguments = {})
{
    auto options = Processes::ProcessOptions {};
    options.executable = EACP_PENDING_REPORTS_PROBE;
    options.arguments = {"--database", database.str()};

    for (const auto& argument: extraArguments)
        options.arguments.add(argument);

    return Processes::run(options);
}

auto tCrashReportStopEndsTheHandler = test("CrashReport/stopEndsTheHandler") = []
{
    auto directory = TemporaryFile {};
    auto result = runProbe(directory.path(), {"--stop"});

    check(result.exitCode == 0);
    check(countLines(result.output, "handler before stop: 1") == 1);
    check(countLines(result.output, "started after stop: 0") == 1);
    check(countLines(result.output, "handler after stop: 0") == 1);
};

auto tCrashReportCrashedLastRunFollowsTheMarker =
    test("CrashReport/crashedLastRunFollowsTheMarker") = []
{
    auto directory = TemporaryFile {};
    auto database = ReportDatabase {directory.path()};

    check(countLines(runProbe(directory.path()).output, "crashed last run: 0") == 1);

    database.markCrashedLastRun();
    check(countLines(runProbe(directory.path(), {"--clear-crashed-last-run"}).output,
                     "crashed last run: 1")
          == 1);
    check(countLines(runProbe(directory.path()).output, "crashed last run: 0") == 1);
};

auto tCrashReportPendingReportsArriveOnTheLoop =
    test("CrashReport/pendingReportsArriveOnTheLoop") = []
{
    auto directory = TemporaryFile {};
    auto database = ReportDatabase {directory.path()};
    auto ids = Vector<std::string> {};

    for (auto index = 0; index < pendingReportCount; ++index)
        ids.add(writePendingReport(database));

    auto result =
        runProbe(directory.path(), {"--expect", std::to_string(pendingReportCount)});
    check(result.launched);
    check(result.exitCode == 0);
    check(countLines(result.output, "during start: 0") == 1);

    for (const auto& id: ids)
        check(countLines(result.output, "delivered: " + id) == 1);
};
