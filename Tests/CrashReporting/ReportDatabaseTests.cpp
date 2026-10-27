#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/CrashReporting/ReportDatabase.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::CrashReporting;

namespace
{
CrashReport makeDatabaseReport()
{
    auto report = CrashReport {};
    report.id = createReportId();
    report.reason = "SIGABRT";
    report.platform = getPlatformName();

    return report;
}
} // namespace

auto tCrashReportDatabaseReadsAnIncompleteFolder =
    test("CrashReport/databaseReadsAnIncompleteFolder") = []
{
    auto directory = TemporaryFile {};
    auto database = ReportDatabase {directory.path()};
    auto id = createReportId();
    database.createReportFolder(id);

    auto reports = database.readPendingReports();

    check(reports.size() == 1);
    check(reports[0].id == id);
    check(reports[0].reason == incompleteReason);
    check(reports[0].capturedBy == CaptureSource::incomplete);
    check(Files::exists(database.getReportFolder(id) / "report.json"));
};

auto tCrashReportDatabaseMarksAReportHandled =
    test("CrashReport/databaseMarksAReportHandled") = []
{
    auto directory = TemporaryFile {};
    auto database = ReportDatabase {directory.path()};
    auto report = makeDatabaseReport();
    database.createReportFolder(report.id);
    check(database.writeReport(report));

    check(database.markReportHandled(report.id, 1));
    check(database.readPendingReports().empty());
    check(Files::exists(database.getHandledDirectory() / report.id / "report.json"));
};

auto tCrashReportDatabasePrunesHandledReports =
    test("CrashReport/databasePrunesHandledReports") = []
{
    auto directory = TemporaryFile {};
    auto database = ReportDatabase {directory.path()};
    constexpr auto reportCount = 3;
    constexpr auto reportsToKeep = 2;

    for (auto index = 0; index < reportCount; ++index)
    {
        auto report = makeDatabaseReport();
        report.timestamp = index;
        database.createReportFolder(report.id);
        database.writeReport(report);
        database.markReportHandled(report.id, reportsToKeep);
    }

    auto handled = Files::listDirectory(database.getHandledDirectory());
    check(handled.size() == reportsToKeep);
};
