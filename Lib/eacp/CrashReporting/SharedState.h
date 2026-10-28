#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/FilePath.h>
#include <eacp/CrashReporting/CrashReport.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// The block of memory the app and its handler process both map. The app writes
// metadata and attachments into it as they are set, so nothing is serialised
// at crash time, and the crash path writes only fixed-size fields. Plain data
// throughout: it is the same bytes in two address spaces.
namespace eacp::CrashReporting::Detail
{
constexpr auto sharedStateMagic = std::uint32_t {0xEAC9C4A5};
constexpr auto sharedStateVersion = std::uint32_t {5};

constexpr auto maxMetadataEntries = std::size_t {64};
constexpr auto maxBreadcrumbs = std::size_t {64};
constexpr auto maxBreadcrumbCategoryLength = std::size_t {64};
constexpr auto maxBreadcrumbMessageLength = std::size_t {512};
constexpr auto maxUserFieldLength = std::size_t {256};
constexpr auto maxLevelNameLength = std::size_t {16};
constexpr auto maxMetadataKeyLength = std::size_t {64};
constexpr auto maxMetadataValueLength = std::size_t {512};
constexpr auto maxAttachments = std::size_t {16};
constexpr auto maxAttachmentPathLength = std::size_t {1024};
constexpr auto maxReasonLength = std::size_t {64};
constexpr auto maxCrashMessageLength = std::size_t {1024};
constexpr auto maxCrashFrames = std::size_t {128};
constexpr auto maxSharedModules = std::size_t {1024};
constexpr auto maxModulePathLength = std::size_t {512};

template <std::size_t Capacity>
using BoundedText = std::array<char, Capacity>;

struct SharedMetadataEntry
{
    BoundedText<maxMetadataKeyLength> key;
    BoundedText<maxMetadataValueLength> value;
};

struct SharedKeyValueTable
{
    std::uint32_t count;
    std::array<SharedMetadataEntry, maxMetadataEntries> entries;
};

struct SharedUser
{
    std::uint32_t isSet;
    BoundedText<maxUserFieldLength> id;
    BoundedText<maxUserFieldLength> email;
    BoundedText<maxUserFieldLength> username;
};

struct SharedBreadcrumb
{
    std::int64_t timestamp;
    BoundedText<maxLevelNameLength> level;
    BoundedText<maxBreadcrumbCategoryLength> category;
    BoundedText<maxBreadcrumbMessageLength> message;
};

// A ring: the newest maxBreadcrumbs of `added`, the slot of the next one
// being added % maxBreadcrumbs.
struct SharedBreadcrumbRing
{
    std::uint64_t added;
    std::array<SharedBreadcrumb, maxBreadcrumbs> entries;
};

struct SharedModule
{
    std::uint64_t base;
    std::uint64_t size;
    BoundedText<maxModulePathLength> path;
};

// What the crashing process writes about its own crash. On macOS that is the
// whole capture: its own stack, walked on the alternate signal stack, and the
// images loaded at the time. On Windows the handler walks the stack from the
// EXCEPTION_POINTERS address, so the frames and modules stay empty there.
struct SharedCrashInfo
{
    BoundedText<maxReasonLength> reason;
    BoundedText<maxCrashMessageLength> message;
    std::uint64_t code;
    std::uint64_t faultAddress;
    std::uint64_t threadId;
    std::uint64_t exceptionPointersAddress;

    std::uint32_t frameCount;
    std::array<std::uint64_t, maxCrashFrames> frames;

    // The call stack at the `throw` of the exception that crashed the app.
    // Written by the app on macOS when it throws; read by the handler process
    // to put the throw site in the report. Empty when the crash was not an
    // uncaught exception, or on Windows.
    std::uint32_t throwFrameCount;
    std::array<std::uint64_t, maxCrashFrames> throwFrames;

    std::uint32_t moduleCount;
    std::array<SharedModule, maxSharedModules> modules;
};

// The main thread's heartbeat and the hang it stops beating in. The app
// writes the heartbeat; whoever watches it (the app's watchdog thread on macOS,
// the handler on Windows) samples the main thread into `frames` and reports.
struct SharedHangInfo
{
    // Milliseconds on the steady clock, which both processes share; 0 until
    // the first beat, so nothing is watched before the loop runs.
    std::atomic<std::uint64_t> heartbeatMilliseconds;

    // pauseHangDetection's nesting depth: nothing is reported while above 0.
    std::atomic<std::uint32_t> pauseDepth;

    // 0 is detection off.
    std::uint64_t thresholdMilliseconds;
    std::uint64_t pollIntervalMilliseconds;

    // The OS's id of the thread start() ran on.
    std::uint64_t mainThreadId;

    // Written by whoever sampled the hang, before the handler is told.
    std::uint64_t durationMilliseconds;
    std::uint32_t frameCount;
    std::array<std::uint64_t, maxCrashFrames> frames;
};

struct SharedState
{
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t appProcessId;

    // Odd while the app is writing the tables below, so a reader that sees an
    // odd count, or a different count after reading, reads again.
    std::atomic<std::uint32_t> tableSequence;

    SharedKeyValueTable metadata;
    SharedKeyValueTable tags;
    SharedKeyValueTable extra;
    SharedUser user;
    SharedBreadcrumbRing breadcrumbs;

    std::uint32_t attachmentCount;
    std::array<BoundedText<maxAttachmentPathLength>, maxAttachments> attachments;

    // Set by stop() before it wakes the handler, which then exits cleanly.
    std::atomic<std::uint32_t> stopRequested;

    SharedCrashInfo crash;
    SharedHangInfo hang;
};

void initialiseSharedState(SharedState& state, std::uint64_t appProcessId);
bool isSharedStateValid(const SharedState& state);

// Both called by the app under its own lock: one writer only. False when the
// table is full or the text does not fit.
bool writeSharedEntry(SharedState& state,
                      SharedKeyValueTable& table,
                      std::string_view key,
                      std::string_view value);
bool removeSharedEntry(SharedState& state,
                       SharedKeyValueTable& table,
                       std::string_view key);
bool writeSharedAttachment(SharedState& state, const FilePath& path);
bool writeSharedUser(SharedState& state, const User& user);
void clearSharedUser(SharedState& state);

// Text longer than its field is cut, as a breadcrumb is never refused.
void addSharedBreadcrumb(SharedState& state,
                         const Breadcrumb& breadcrumb,
                         std::int64_t timestamp);

// The handler's side, after the crash, so the app is no longer writing.
std::map<std::string, std::string>
    readSharedEntries(const SharedState& state, const SharedKeyValueTable& table);
Vector<FilePath> readSharedAttachments(const SharedState& state);
User readSharedUser(const SharedState& state);

// Oldest first.
Vector<Breadcrumb> readSharedBreadcrumbs(const SharedState& state);

// The steady clock the heartbeat is measured on, in milliseconds.
std::uint64_t getSteadyMilliseconds();

// How long the heartbeat has been silent when that is a hang to report: the
// heartbeat has started, detection is on and not paused, and the silence is
// at least the threshold past the beat that was due. 0 otherwise.
std::uint64_t findHangDuration(const SharedHangInfo& hang, std::uint64_t now);

// Async-signal-safe: copies into a fixed field without allocating.
void copyBoundedText(char* destination, std::size_t capacity, std::string_view text);
std::string_view readBoundedText(const char* source, std::size_t capacity);

template <std::size_t Capacity>
void copyBoundedText(BoundedText<Capacity>& destination, std::string_view text)
{
    copyBoundedText(destination.data(), Capacity, text);
}

template <std::size_t Capacity>
std::string_view readBoundedText(const BoundedText<Capacity>& source)
{
    return readBoundedText(source.data(), Capacity);
}

// The module the address falls in, as an index into crash.modules.
std::optional<std::size_t> findSharedModuleIndex(const SharedCrashInfo& crash,
                                                 std::uint64_t address);
} // namespace eacp::CrashReporting::Detail
