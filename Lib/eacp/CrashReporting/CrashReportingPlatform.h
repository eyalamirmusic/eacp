#pragma once

#include "ReportDatabase.h"
#include "SharedState.h"

#include <chrono>

// What each platform's client and handler process implement, and the portable
// pieces of the handler they share.
namespace eacp::CrashReporting::Detail
{
struct HandlerLaunch
{
    FilePath executable;
    FilePath databaseDirectory;

    // The prefix of every named object the platform makes for this session.
    std::string channelName;
    Vector<std::string> handlerArguments;
    std::chrono::milliseconds startTimeout {};
};

// The handler's command line after the flag: the database, then whatever the
// platform's client put in `channel` to find the app and the shared block.
struct HandlerArguments
{
    FilePath databaseDirectory;
    std::string channel;
};

// Memory both processes map, valid for the life of the app.
SharedState& createSharedState(const HandlerLaunch& launch);

// Launches the handler and waits up to launch.startTimeout for it to be ready.
// False when it never was.
bool launchHandlerProcess(const HandlerLaunch& launch, SharedState& state);

// The in-app handlers, which hand a crash to the handler process. With the
// handler gone the crash is re-raised and lost.
void installCrashHandlers(SharedState& state);

bool isHandlerProcessRunning();

// stop()'s halves: the platform's own crash handling back, then the handler
// told to exit cleanly and waited for.
void uninstallCrashHandlers();
void stopHandlerProcess(SharedState& state);

int runHandlerProcess(const HandlerArguments& arguments);

std::string getOperatingSystemVersion();
std::uint64_t getCurrentProcessId();

// Launches `<executable> <handlerFlag> <database> <channel> -- <arguments>`.
Vector<std::string> makeHandlerCommandLine(const HandlerLaunch& launch,
                                           std::string_view channel);

// The handler's steps once the crash is captured: the report from the shared
// block, with `frames` and the platform's reason filled in by the caller.
CrashReport beginHandlerReport(const ReportDatabase& database,
                               const SharedState& state,
                               CaptureSource capturedBy);

// Copies the attachments and writes report.json, which goes last.
bool finishHandlerReport(const ReportDatabase& database,
                         CrashReport& report,
                         const SharedState& state);

// crashReported, catching whatever it throws: the handler's exit code.
int deliverReportInHandler(const CrashReport& report);
} // namespace eacp::CrashReporting::Detail
