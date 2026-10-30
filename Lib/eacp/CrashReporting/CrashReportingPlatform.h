#pragma once

#include "ReportDatabase.h"
#include "SharedState.h"

#include <eacp/Core/Utils/Time.h>

#include <typeinfo>
#include <functional>
#include <optional>

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
    Time::MS startTimeout {};
    bool captureMachExceptions = true;

    // Windows lists its Windows Error Reporting helper DLL in HKCU; ignored elsewhere.
    bool shouldListCrashHelperWithWindowsErrorReporting = false;
    std::optional<std::uint32_t> requiredWindowsErrorReportingHelperVersion;
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
// handler gone the crash goes unreported, and the next start() sees the
// session marker it left (CrashHandler::unreportedExit).
void installCrashHandlers(SharedState& state);

// False when the thread had a stack for overflow reports already.
bool installStackOverflowStackOnCurrentThread();
void setStackOverflowStackOnNewThreads(bool shouldInstall);

// Through the same per-thread hook: each thread that ends frees its watch slot.
void setReleasesWatchOnThreadEnd(bool shouldRelease);
void releaseWatchOfThread(std::uint64_t threadId);

bool isHandlerProcessRunning();

// Ends the handler process without telling it, as if it had died: the crash
// faults library's way to exercise an unreported exit.
void killHandlerProcess();

// stop()'s halves: the platform's own crash handling back, then the handler
// told to exit cleanly and waited for.
void uninstallCrashHandlers();
void stopHandlerProcess(SharedState& state);

int runHandlerProcess(const HandlerArguments& arguments);

// The app's side of hang detection: on macOS a watchdog thread that samples
// the main thread itself, on Windows nothing, since the handler watches.
void startHangWatchdog(SharedState& state);
void stopHangWatchdog();

// The calling thread's stack, symbolised in the app, its innermost
// `framesToSkip` frames left out.
Vector<StackFrame> captureCurrentStack(int framesToSkip);

// The type's name as source spells it: demangled, with no "class " prefix.
std::string getTypeName(const std::type_info& type);
// A duration as the shared block stores it: whole milliseconds, never negative.
std::uint64_t countMilliseconds(Time::MS duration);

// The block start() made, while the session is started and watching for hangs.
std::optional<std::reference_wrapper<SharedHangInfo>> findWatchedHangInfo();

// Whether a thread's first sendHeartbeat claims it a slot.
bool isWatchingWorkerThreads();

// Makes the calling thread's heartbeat, pause and resume act on the main slot.
void useMainSlotOnCurrentThread();

// Records what the in-app watchdog needs to sample the calling thread later
// (macOS); nothing on Windows.
void recordThreadForSampling(SharedWatchedThread& thread);

std::string getOperatingSystemVersion();
std::uint64_t getCurrentProcessId();

// The OS's id for the calling thread, as a report names it.
std::uint64_t getCurrentThreadId();

// The name the OS holds for the calling thread; empty when it has none.
std::string getCurrentThreadName();
void setCurrentThreadName(std::string_view name);

// Launches `<executable> <handlerFlag> <database> <channel> -- <arguments>`.
Vector<std::string> makeHandlerCommandLine(const HandlerLaunch& launch,
                                           std::string_view channel);

// The handler's steps once the crash is captured: the report from the shared
// block, with `frames` and the platform's reason filled in by the caller.
CrashReport beginHandlerReport(const ReportDatabase& database,
                               const SharedState& state,
                               CaptureSource capturedBy);

// beginHandlerReport for a hang: the kind, reason and message, one
// stalledThreads entry per stalled slot (id and name) and how long the first
// had been silent. The caller fills each entry's frames, then
// finishHangReportThreads copies the first into crashingThread.
CrashReport beginHangReport(const ReportDatabase& database,
                            const SharedState& state);

void finishHangReportThreads(CrashReport& report);

// Copies the attachments and writes report.json, which goes last. Only a crash
// marks crashedLastRun.
bool finishHandlerReport(const ReportDatabase& database,
                         CrashReport& report,
                         const SharedState& state);

// reportWritten, catching whatever it throws: the handler's exit code.
int deliverReportInHandler(const CrashReport& report);
} // namespace eacp::CrashReporting::Detail
