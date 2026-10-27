#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/FilePath.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace eacp::CrashReporting
{
// One return address on the crashed thread, as the module it falls in and the
// distance into it, which stays meaningful across runs and machines where the
// absolute address does not.
struct StackFrame
{
    std::string modulePath;
    std::uint64_t moduleBase = 0;
    std::uint64_t offset = 0;

    // Empty when nothing resolved it.
    std::string symbol;

    // The key a symbol server finds the image's symbols by: the Mach-O LC_UUID
    // on macOS, the PDB's GUID and age on Windows. Empty
    // where it was not read.
    std::string moduleUuid;

    std::uint64_t getAddress() const { return moduleBase + offset; }
};

struct CrashedThread
{
    std::uint64_t threadId = 0;
    Vector<StackFrame> frames;
};

// Who wrote the report: the handler process reading the crashed one from
// outside, the handler again after the app was gone with only its exit code to go on,
// or nobody, for a folder a crash left without its report.json.
enum class CaptureSource
{
    handler,
    exitCode,
    incomplete
};

std::string_view getCaptureSourceName(CaptureSource source);
std::optional<CaptureSource> findCaptureSource(std::string_view name);

constexpr auto incompleteReason = "incomplete";
constexpr auto hangReason = "Hang";

// A crash ended the app; a hang is a main thread that stopped answering, and
// the app was left running.
enum class ReportKind
{
    crash,
    hang
};

std::string_view getReportKindName(ReportKind kind);
std::optional<ReportKind> findReportKind(std::string_view name);

enum class Level
{
    debug,
    info,
    warning,
    error,
    fatal
};

std::string_view getLevelName(Level level);
std::optional<Level> findLevel(std::string_view name);

// Who was using the app, as setUser gave it. All empty when none was set.
struct User
{
    std::string id;
    std::string email;
    std::string username;
};

struct Breadcrumb
{
    std::string category;
    std::string message;
    Level level = Level::info;

    // Milliseconds since the Unix epoch, UTC, stamped by addBreadcrumb.
    std::int64_t timestamp = 0;
};

struct CrashReport
{
    // A UUID, and the name of the report's folder.
    std::string id;

    ReportKind kind = ReportKind::crash;

    // Milliseconds since the Unix epoch, UTC.
    std::int64_t timestamp = 0;

    // The platform's own name for what happened: EXC_BAD_ACCESS, SIGABRT,
    // EXCEPTION_ACCESS_VIOLATION, std::terminate, incomplete.
    std::string reason;

    // What the crash said about itself: an uncaught exception's type and what(),
    // an NSException's name and reason. Often empty.
    std::string message;

    std::uint64_t faultAddress = 0;

    // For a hang, the main thread; its frames are where it was stuck.
    CrashedThread crashingThread;

    // How long the main thread had been silent when it was sampled, for a
    // hang. 0 for a crash.
    std::int64_t hangDurationMilliseconds = 0;

    // Empty where no minidump was written (macOS has no writer yet).
    FilePath minidumpPath;

    // The module's own keys: appName, appVersion, os, release, environment,
    // dist.
    std::map<std::string, std::string> metadata;

    // What the app set with setTag and setExtra.
    std::map<std::string, std::string> tags;
    std::map<std::string, std::string> extra;

    User user;

    // Oldest first.
    Vector<Breadcrumb> breadcrumbs;

    Vector<FilePath> attachments;

    // "macOS" or "Windows".
    std::string platform;

    CaptureSource capturedBy = CaptureSource::handler;

    // The folder the report lives in, filled in when it is read back. Not
    // stored, since the folder moves when the report is marked handled.
    FilePath folder;

    std::string toJSON() const;
    static std::optional<CrashReport> fromJSON(std::string_view text);
};

// Addresses go through JSON as strings, because a JSON number is a double and
// loses the low bits of anything above 2^53.
std::string formatHexAddress(std::uint64_t address);
std::optional<std::uint64_t> parseHexAddress(std::string_view text);
} // namespace eacp::CrashReporting
