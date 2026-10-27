#include "CrashReport.h"

#include <Miro/Json.h>
#include <Miro/Reflect.h>

#include <array>
#include <charconv>
#include <exception>

namespace eacp::CrashReporting
{
namespace
{
constexpr auto hexPrefix = std::string_view {"0x"};
constexpr auto hexBase = 16;
constexpr auto reportIndent = 2;

struct CaptureSourceName
{
    CaptureSource source;
    std::string_view name;
};

constexpr auto captureSourceNames = std::to_array<CaptureSourceName>({
    {CaptureSource::handler, "handler"},
    {CaptureSource::exitCode, "exit-code"},
    {CaptureSource::incomplete, "incomplete"},
});

struct LevelName
{
    Level level;
    std::string_view name;
};

constexpr auto levelNames = std::to_array<LevelName>({
    {Level::debug, "debug"},
    {Level::info, "info"},
    {Level::warning, "warning"},
    {Level::error, "error"},
    {Level::fatal, "fatal"},
});

struct UserRecord
{
    std::string id;
    std::string email;
    std::string username;

    MIRO_REFLECT(id, email, username)
};

struct BreadcrumbRecord
{
    std::string category;
    std::string message;
    std::string level;
    std::int64_t timestamp = 0;

    MIRO_REFLECT(category, message, level, timestamp)
};

struct StackFrameRecord
{
    std::string modulePath;
    std::string moduleBase;
    std::string offset;
    std::string symbol;
    std::string moduleUuid;

    MIRO_REFLECT(modulePath, moduleBase, offset, symbol, moduleUuid)
};

struct CrashedThreadRecord
{
    std::string threadId;
    Vector<StackFrameRecord> frames;

    MIRO_REFLECT(threadId, frames)
};

struct CrashReportRecord
{
    std::string id;
    std::int64_t timestamp = 0;
    std::string reason;
    std::string message;
    std::string faultAddress;
    CrashedThreadRecord crashingThread;
    std::string minidumpPath;
    std::map<std::string, std::string> metadata;
    std::map<std::string, std::string> tags;
    std::map<std::string, std::string> extra;
    UserRecord user;
    Vector<BreadcrumbRecord> breadcrumbs;
    Vector<std::string> attachments;
    std::string platform;
    std::string capturedBy;

    MIRO_REFLECT(id,
                 timestamp,
                 reason,
                 message,
                 faultAddress,
                 crashingThread,
                 minidumpPath,
                 metadata,
                 tags,
                 extra,
                 user,
                 breadcrumbs,
                 attachments,
                 platform,
                 capturedBy)
};

StackFrameRecord makeStackFrameRecord(const StackFrame& frame)
{
    return {frame.modulePath,
            formatHexAddress(frame.moduleBase),
            formatHexAddress(frame.offset),
            frame.symbol,
            frame.moduleUuid};
}

StackFrame makeStackFrame(const StackFrameRecord& record)
{
    return {record.modulePath,
            parseHexAddress(record.moduleBase).value_or(0),
            parseHexAddress(record.offset).value_or(0),
            record.symbol,
            record.moduleUuid};
}

CrashReportRecord makeCrashReportRecord(const CrashReport& report)
{
    auto record = CrashReportRecord {};
    record.id = report.id;
    record.timestamp = report.timestamp;
    record.reason = report.reason;
    record.message = report.message;
    record.faultAddress = formatHexAddress(report.faultAddress);
    record.crashingThread.threadId =
        formatHexAddress(report.crashingThread.threadId);

    for (const auto& frame: report.crashingThread.frames)
        record.crashingThread.frames.add(makeStackFrameRecord(frame));

    record.minidumpPath = report.minidumpPath.str();
    record.metadata = report.metadata;
    record.tags = report.tags;
    record.extra = report.extra;
    record.user = {report.user.id, report.user.email, report.user.username};

    for (const auto& breadcrumb: report.breadcrumbs)
        record.breadcrumbs.add({breadcrumb.category,
                                breadcrumb.message,
                                std::string {getLevelName(breadcrumb.level)},
                                breadcrumb.timestamp});

    for (const auto& attachment: report.attachments)
        record.attachments.add(attachment.str());

    record.platform = report.platform;
    record.capturedBy = std::string {getCaptureSourceName(report.capturedBy)};

    return record;
}

CrashReport makeCrashReport(const CrashReportRecord& record)
{
    auto report = CrashReport {};
    report.id = record.id;
    report.timestamp = record.timestamp;
    report.reason = record.reason;
    report.message = record.message;
    report.faultAddress = parseHexAddress(record.faultAddress).value_or(0);
    report.crashingThread.threadId =
        parseHexAddress(record.crashingThread.threadId).value_or(0);

    for (const auto& frame: record.crashingThread.frames)
        report.crashingThread.frames.add(makeStackFrame(frame));

    report.minidumpPath = FilePath {record.minidumpPath};
    report.metadata = record.metadata;
    report.tags = record.tags;
    report.extra = record.extra;
    report.user = {record.user.id, record.user.email, record.user.username};

    for (const auto& breadcrumb: record.breadcrumbs)
        report.breadcrumbs.add({breadcrumb.category,
                                breadcrumb.message,
                                findLevel(breadcrumb.level).value_or(Level::info),
                                breadcrumb.timestamp});

    for (const auto& attachment: record.attachments)
        report.attachments.add(FilePath {attachment});

    report.platform = record.platform;
    report.capturedBy =
        findCaptureSource(record.capturedBy).value_or(CaptureSource::handler);

    return report;
}
} // namespace

std::string_view getLevelName(Level level)
{
    for (const auto& entry: levelNames)
        if (entry.level == level)
            return entry.name;

    return {};
}

std::optional<Level> findLevel(std::string_view name)
{
    for (const auto& entry: levelNames)
        if (entry.name == name)
            return entry.level;

    return std::nullopt;
}

std::string_view getCaptureSourceName(CaptureSource source)
{
    for (const auto& entry: captureSourceNames)
        if (entry.source == source)
            return entry.name;

    return {};
}

std::optional<CaptureSource> findCaptureSource(std::string_view name)
{
    for (const auto& entry: captureSourceNames)
        if (entry.name == name)
            return entry.source;

    return std::nullopt;
}

std::string CrashReport::toJSON() const
{
    return Miro::toJSONString(makeCrashReportRecord(*this), reportIndent);
}

std::optional<CrashReport> CrashReport::fromJSON(std::string_view text)
{
    try
    {
        auto record = Miro::createFromJSONString<CrashReportRecord>(text);

        if (record.id.empty())
            return std::nullopt;

        return makeCrashReport(record);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::string formatHexAddress(std::uint64_t address)
{
    auto digits = Array<char, 32> {};
    auto [end, error] = std::to_chars(
        digits.data(), digits.data() + digits.size(), address, hexBase);

    return std::string {hexPrefix} + std::string {digits.data(), end};
}

std::optional<std::uint64_t> parseHexAddress(std::string_view text)
{
    if (text.starts_with(hexPrefix))
        text.remove_prefix(hexPrefix.size());

    if (text.empty())
        return std::nullopt;

    auto value = std::uint64_t {0};
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, hexBase);

    if (error != std::errc {} || end != text.data() + text.size())
        return std::nullopt;

    return value;
}
} // namespace eacp::CrashReporting
