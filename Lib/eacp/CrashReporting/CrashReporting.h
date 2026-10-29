#pragma once

#include "CrashHandler.h"
#include "CrashReport.h"
#include "ReportDatabase.h"

#include <eacp/Core/Platform/Platform.h>

#include <chrono>
#include <optional>
#include <string_view>

// Crash reporting for an executable, with the report written by a second
// process. main() starts with
//
//   if (auto code = CrashReporting::runHandlerIfRequested(argc, argv))
//       return *code;
//
//   CrashReporting::start();
//
// The first call is the handler process's whole life: start() relaunches the
// app's own executable with a flag this recognises, so the app's own argument
// parsing never sees it. The second installs the reporter in the app and
// launches that handler, which waits for a crash, writes the report folder,
// ends the app and hands the report to the registered CrashHandler.
namespace eacp::CrashReporting
{
// A stack overflow leaves its thread no stack for the crash handler, so the
// report needs a separate one set up on the thread beforehand: an alternate
// signal stack on macOS, a SetThreadStackGuarantee reserve on Windows. Each
// stack is reserved address space, not resident memory, until a crash uses it.
enum class StackOverflowReporting
{
    // The thread that calls start(), usually the main thread.
    startingThread,

    // That thread and every thread created after start(), whoever creates it:
    // std::thread, Threads::Thread, GCD and the Windows thread pool, other
    // libraries. Costs every new thread a reservation and a system call or two.
    everyThread,

    off
};

constexpr auto handlerFlag = std::string_view {"--eacp-crash-handler"};
constexpr auto handlerArgumentsSeparator = std::string_view {"--"};

FilePath getDefaultDatabaseDirectory();

// The app's identity (company and app name, as FilePath::appSupportDirectory
// names its folder), the process id and a random suffix: the prefix of the
// named objects a session makes where the platform names them (Windows).
std::string createDefaultChannelName();

struct Options
{
    FilePath databaseDirectory = getDefaultDatabaseDirectory();

    // handled/ keeps this many reports, the newest.
    int maxHandledReports = 20;

    // The handler process's executable; empty is this one.
    FilePath handlerExecutable;

    // How long start() waits for the handler to come up before going on
    // without it, in which case a crash goes unreported.
    std::chrono::milliseconds handlerStartTimeout {5000};

    // Empty is createDefaultChannelName().
    std::string channelName;

    // The handler process's command line after the executable, which is what
    // Apps::commandLineArgs() holds there.
    Vector<std::string> handlerArguments;

    // Written into the report's metadata under these keys. release defaults
    // to the app's version from its AppInfo.
    std::string release = std::string {Platform::getAppVersion()};
    std::string environment;
    std::string dist;

    // Added by start() as addAttachment would.
    Vector<FilePath> attachments;

    // How often the main thread's heartbeat is written, from a timer on the
    // event loop, and how long it may go silent before that is reported as a
    // hang. A zero threshold turns hang detection off.
    std::chrono::milliseconds heartbeatInterval {500};
    std::chrono::milliseconds hangThreshold {10000};

    StackOverflowReporting stackOverflowReporting =
        StackOverflowReporting::startingThread;
};

// The handler process's entry: its exit code when argv is the handler's command
// line, nullopt otherwise.
std::optional<int> runHandlerIfRequested(int argc, char* argv[]);

// True in the handler process, from runHandlerIfRequested on.
bool isHandlerProcess();

// Installs the crash handlers, launches the handler process and schedules
// pendingReportFound on the event loop for each report left over from earlier
// runs; it returns without calling any of them. False when the
// handler could not be started; crashes then go unreported, and
// handlerUnavailable is scheduled the same way. Writes the session marker,
// which stop() and a normal exit remove, and schedules unreportedExit when the
// last run's marker is still there with no crash report since it started.
// Call once, early in main().
bool start(const Options& options = {});

// Tells the handler to exit without a report, removes the session marker,
// waits for the handler, and puts the
// platform's own crash handling back. isStarted() is false after, and start()
// is refused for the rest of the process.
void stop();

bool isStarted();

// Whether the handler process start() launched is still there to catch a
// crash. False before start() and once it has died.
bool isHandlerRunning();

// Each writes into memory the handler process already maps, so it is what the
// report says at the moment of the crash with nothing serialised then. A tag or
// extra key of up to 63 bytes and a value of up to 511, up to 64 of each, a
// user field of up to 255 and up to 16 attachments; false when one does not
// fit. Attachments are copied into the report's folder when it is written.
// All false before start().
bool setTag(std::string_view key, std::string_view value);
bool removeTag(std::string_view key);
bool setExtra(std::string_view key, std::string_view value);
bool setUser(const User& user);
void clearUser();
bool addAttachment(const FilePath& path);

// For main-thread work that is meant to take longer than the hang threshold.
// Nests; the silence is measured afresh from resumeHangDetection.
void pauseHangDetection();
void resumeHangDetection();

// Kept in a ring of the newest Detail::maxBreadcrumbs, timestamped now. A
// category longer than 63 bytes or a message longer than 511 is cut.
void addBreadcrumb(const Breadcrumb& breadcrumb);

// Whether the last run wrote a report, as start() found the database. Stays
// true on later launches until clearCrashedLastRun().
bool crashedLastRun();
void clearCrashedLastRun();

// Moves the report from pending/ to handled/, so pendingReportFound stops
// seeing it, and prunes handled/.
bool markReportHandled(std::string_view id);

// The database start() was given; the default before it is called.
ReportDatabase getDatabase();
} // namespace eacp::CrashReporting
