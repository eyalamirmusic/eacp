#include "CrashReporting.h"
#include "CrashReportingPlatform.h"

#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Time.h>

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
    report.timestamp = Time::sinceEpoch().count;
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

    if (report.kind == ReportKind::crash)
        database.markCrashedLastRun();

    return database.writeReport(report);
}

CrashReport beginHangReport(const ReportDatabase& database, const SharedState& state)
{
    constexpr auto millisecondsPerSecond = std::uint64_t {1000};
    constexpr auto millisecondsPerTenth = std::uint64_t {100};
    auto report = beginHandlerReport(database, state, CaptureSource::handler);
    auto duration = state.hang.durationMilliseconds;
    auto seconds =
        std::to_string(duration / millisecondsPerSecond) + "."
        + std::to_string(duration % millisecondsPerSecond / millisecondsPerTenth);

    report.kind = ReportKind::hang;
    report.reason = hangReason;
    report.message = "main thread unresponsive for " + seconds + " s";
    report.faultAddress = 0;
    report.crashingThread.threadId = state.hang.mainThreadId;
    report.hangDurationMilliseconds =
        static_cast<std::int64_t>(state.hang.durationMilliseconds);

    return report;
}

int deliverReportInHandler(const CrashReport& report)
{
    try
    {
        auto handler = createRegisteredCrashHandler();

        if (report.kind == ReportKind::hang)
            handler->hangReported(report);
        else
            handler->crashReported(report);

        return 0;
    }
    catch (const std::exception& error)
    {
        Console::printError(std::string {"the crash handler threw: "}
                            + error.what());
    }
    catch (...)
    {
        Console::printError("the crash handler threw");
    }

    return 1;
}
} // namespace eacp::CrashReporting::Detail
