#include "CrashReporting.h"
#include "CrashReportingPlatform.h"

#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Format.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Utils/Time.h>

#include <algorithm>

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

namespace
{
std::string formatTenthsOfSeconds(std::uint64_t milliseconds)
{
    constexpr auto millisecondsPerSecond = std::uint64_t {1000};
    constexpr auto millisecondsPerTenth = std::uint64_t {100};

    return Strings::format("{}.{}",
                           milliseconds / millisecondsPerSecond,
                           milliseconds % millisecondsPerSecond
                               / millisecondsPerTenth);
}

std::string describeStalledThreads(const Vector<CrashedThread>& threads)
{
    auto names = EA::Vectors::transform(
        threads, [](const CrashedThread& thread) { return thread.name; });

    auto noun = threads.size() == 1 ? " thread" : " threads";
    return Strings::join(names, ", ") + noun;
}
} // namespace

CrashReport beginHangReport(const ReportDatabase& database, const SharedState& state)
{
    auto report = beginHandlerReport(database, state, CaptureSource::handler);
    const auto& hang = state.hang;
    auto count =
        std::min(static_cast<std::size_t>(hang.stalledCount), maxWatchedThreads);

    for (auto index = std::size_t {0}; index < count; ++index)
    {
        auto thread = CrashedThread {};
        thread.threadId = hang.stalled[index].threadId;
        thread.name = std::string {readBoundedText(hang.stalled[index].name)};
        report.stalledThreads.add(thread);
    }

    auto duration = count > 0 ? hang.stalled[0].durationMilliseconds : 0;

    report.kind = ReportKind::hang;
    report.reason = hangReason;
    report.message = describeStalledThreads(report.stalledThreads)
                     + " unresponsive for " + formatTenthsOfSeconds(duration) + " s";
    report.faultAddress = 0;
    report.hangDurationMilliseconds = static_cast<std::int64_t>(duration);
    finishHangReportThreads(report);

    return report;
}

void finishHangReportThreads(CrashReport& report)
{
    if (report.stalledThreads.empty())
        return;

    report.crashingThread = report.stalledThreads.front();
    report.threadName = report.crashingThread.name;
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
