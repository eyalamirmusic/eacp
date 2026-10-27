#include "CrashReporting.h"
#include "CrashReportingPlatform.h"

#include <eacp/Core/Utils/Console.h>

#include <exception>

namespace eacp::CrashReporting::Detail
{
Vector<std::string> makeHandlerCommandLine(const HandlerLaunch& launch,
                                           std::string_view channel)
{
    auto commandLine = Vector<std::string> {launch.executable.str(),
                                            std::string {handlerFlag},
                                            launch.databaseDirectory.str(),
                                            std::string {channel},
                                            std::string {handlerArgumentsSeparator}};

    for (const auto& argument: launch.handlerArguments)
        commandLine.add(argument);

    return commandLine;
}

CrashReport beginHandlerReport(const ReportDatabase& database,
                               const SharedState& state,
                               CaptureSource capturedBy)
{
    auto report = CrashReport {};
    report.id = createReportId();
    report.timestamp = getMillisecondsSinceEpoch();
    report.reason = std::string {readBoundedText(state.crash.reason)};
    report.message = std::string {readBoundedText(state.crash.message)};
    report.faultAddress = state.crash.faultAddress;
    report.crashingThread.threadId = state.crash.threadId;
    report.metadata = readSharedEntries(state, state.metadata);
    report.tags = readSharedEntries(state, state.tags);
    report.extra = readSharedEntries(state, state.extra);
    report.user = readSharedUser(state);
    report.breadcrumbs = readSharedBreadcrumbs(state);
    report.platform = getPlatformName();
    report.capturedBy = capturedBy;
    report.folder = database.createReportFolder(report.id);

    return report;
}

bool finishHandlerReport(const ReportDatabase& database,
                         CrashReport& report,
                         const SharedState& state)
{
    report.attachments =
        database.copyAttachments(report.id, readSharedAttachments(state));

    database.markCrashedLastRun();
    return database.writeReport(report);
}

int deliverReportInHandler(const CrashReport& report)
{
    try
    {
        createRegisteredCrashHandler()->crashReported(report);
        return 0;
    }
    catch (const std::exception& error)
    {
        Console::printError(std::string {"crashReported threw: "} + error.what());
    }
    catch (...)
    {
        Console::printError("crashReported threw");
    }

    return 1;
}
} // namespace eacp::CrashReporting::Detail
