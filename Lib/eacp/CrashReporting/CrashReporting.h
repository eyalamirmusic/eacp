#pragma once

#include "CrashHandler.h"
#include "CrashReport.h"
#include "ReportDatabase.h"

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
// ends the app and hands the report to the registered CrashHandler. See
// README.md beside this header.
namespace eacp::CrashReporting
{
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

    // How long start() waits for the handler to come up before going on
    // without it, in which case a crash goes unreported.
    std::chrono::milliseconds handlerStartTimeout {5000};

    // Empty is createDefaultChannelName().
    std::string channelName;

    // The handler process's command line after the executable, which is what
    // Apps::commandLineArgs() holds there.
    Vector<std::string> handlerArguments;
};

// The handler process's entry: its exit code when argv is the handler's command
// line, nullopt otherwise.
std::optional<int> runHandlerIfRequested(int argc, char* argv[]);

// True in the handler process, from runHandlerIfRequested on.
bool isHandlerProcess();

// Installs the crash handlers, launches the handler process and then calls
// pendingReportFound for reports left over from earlier runs. False when the
// handler could not be started; crashes then go unreported.
// Call once, early in main().
bool start(const Options& options = {});

bool isStarted();

// Whether the handler process start() launched is still there to catch a
// crash. False before start() and once it has died.
bool isHandlerRunning();

// Both write into memory the handler process already maps, so they are what
// the report says at the moment of the crash with nothing serialised then. A
// key of up to 63 bytes, a value of up to 511, up to 64 keys and 16
// attachments; false when one does not fit. Attachments are copied into the
// report's folder when it is written.
bool setMetadata(std::string_view key, std::string_view value);
bool addAttachment(const FilePath& path);

// Moves the report from pending/ to handled/, so pendingReportFound stops
// seeing it, and prunes handled/.
bool markReportHandled(std::string_view id);

// The database start() was given; the default before it is called.
ReportDatabase getDatabase();
} // namespace eacp::CrashReporting
