#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/Core/Utils/Time.h>
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
    report.timestamp = Time::sinceEpoch().count;
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

int countLinesStartingWith(const std::string& rawOutput, std::string_view prefix)
{
    auto count = 0;

    for (const auto& line: Strings::split(removeCarriageReturns(rawOutput), '\n'))
    {
        if (line.starts_with(prefix))
            ++count;
    }

    return count;
}

constexpr auto unreportedExitPrefix = std::string_view {"unreported exit: "};
constexpr auto handUncleanProcessId = std::uint64_t {4242};
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

auto tCrashReportHandlerUnavailable =
    test("CrashReport/handlerUnavailableWhenItCannotLaunch") = []
{
    auto directory = TemporaryFile {};
    auto missingHandler = TemporaryFile {};
    auto result = runProbe(directory.path(),
                           {"--handler-executable", missingHandler.path().str()});

    check(result.exitCode == 0);
    check(countLines(result.output, "started: 0") == 1);
    check(countLinesStartingWith(result.output, "handler unavailable: ") == 1);
    check(ReportDatabase {directory.path()}.readPendingReports().empty());
};

auto tSessionCleanExitLeavesNoMarker =
    test("CrashReport/sessionMarkerGoneAfterACleanExit") = []
{
    auto directory = TemporaryFile {};
    auto database = ReportDatabase {directory.path()};

    check(runProbe(directory.path(), {"--stop"}).exitCode == 0);
    check(!Files::exists(database.getSessionMarker()));

    check(runProbe(directory.path()).exitCode == 0);
    check(!Files::exists(database.getSessionMarker()));
};

auto tSessionUncleanExitIsReportedOnce =
    test("CrashReport/sessionUnreportedExitFiresOnce") = []
{
    auto directory = TemporaryFile {};
    auto database = ReportDatabase {directory.path()};
    auto unclean = AppSession {};
    unclean.processId = handUncleanProcessId;
    unclean.startTime = Time::sinceEpoch().count;
    database.writeSessionMarker(unclean);

    auto first = runProbe(directory.path());
    check(countLines(first.output,
                     std::string {unreportedExitPrefix}
                         + std::to_string(handUncleanProcessId))
          == 1);
    check(countLinesStartingWith(first.output, unreportedExitPrefix) == 1);

    auto second = runProbe(directory.path());
    check(countLinesStartingWith(second.output, unreportedExitPrefix) == 0);
};
