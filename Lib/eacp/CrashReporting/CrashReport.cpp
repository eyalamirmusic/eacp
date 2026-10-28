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
    {CaptureSource::app, "app"},
    {CaptureSource::incomplete, "incomplete"},
});

struct ReportKindName
{
    ReportKind kind;
    std::string_view name;
};

constexpr auto reportKindNames = std::to_array<ReportKindName>({
    {ReportKind::crash, "crash"},
    {ReportKind::hang, "hang"},
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

// Saves `value` as a hex string under `key`, and reads it back.
void reflectHexAddress(Miro::Reflector& ref,
                       std::string_view key,
                       std::uint64_t& value)
{
    auto text = std::string {};

    if (ref.isSaving())
        text = formatHexAddress(value);

    ref[key](text);

    if (ref.isLoading())
        value = parseHexAddress(text).value_or(0);
}

void reflectPath(Miro::Reflector& ref, std::string_view key, FilePath& path)
{
    auto text = path.str();
    ref[key](text);

    if (ref.isLoading())
        path = FilePath {text};
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

std::string_view getReportKindName(ReportKind kind)
{
    for (const auto& entry: reportKindNames)
        if (entry.kind == kind)
            return entry.name;

    return {};
}

std::optional<ReportKind> findReportKind(std::string_view name)
{
    for (const auto& entry: reportKindNames)
        if (entry.name == name)
            return entry.kind;

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

void reflect(Miro::Reflector& ref, StackFrame& frame)
{
    ref["modulePath"](frame.modulePath);
    reflectHexAddress(ref, "moduleBase", frame.moduleBase);
    reflectHexAddress(ref, "offset", frame.offset);
    ref["symbol"](frame.symbol);
    ref["moduleUuid"](frame.moduleUuid);
}

void reflect(Miro::Reflector& ref, CrashedThread& thread)
{
    reflectHexAddress(ref, "threadId", thread.threadId);
    ref["frames"](thread.frames);
}

void reflect(Miro::Reflector& ref, User& user)
{
    ref["id"](user.id);
    ref["email"](user.email);
    ref["username"](user.username);
}

void reflect(Miro::Reflector& ref, Breadcrumb& breadcrumb)
{
    auto level = std::string {getLevelName(breadcrumb.level)};
    ref["category"](breadcrumb.category);
    ref["message"](breadcrumb.message);
    ref["level"](level);
    ref["timestamp"](breadcrumb.timestamp);

    if (ref.isLoading())
        breadcrumb.level = findLevel(level).value_or(Level::info);
}

void reflect(Miro::Reflector& ref, CrashReport& report)
{
    auto kind = std::string {getReportKindName(report.kind)};
    auto capturedBy = std::string {getCaptureSourceName(report.capturedBy)};
    auto attachments = Vector<std::string> {};

    for (const auto& attachment: report.attachments)
        attachments.add(attachment.str());

    ref["id"](report.id);
    ref["kind"](kind);
    ref["timestamp"](report.timestamp);
    ref["reason"](report.reason);
    ref["message"](report.message);
    reflectHexAddress(ref, "faultAddress", report.faultAddress);
    ref["crashingThread"](report.crashingThread);
    ref["terminateFrames"](report.terminateFrames);
    ref["hangDuration"](report.hangDurationMilliseconds);
    reflectPath(ref, "minidumpPath", report.minidumpPath);
    ref["metadata"](report.metadata);
    ref["tags"](report.tags);
    ref["extra"](report.extra);
    ref["user"](report.user);
    ref["breadcrumbs"](report.breadcrumbs);
    ref["attachments"](attachments);
    ref["platform"](report.platform);
    ref["capturedBy"](capturedBy);

    if (!ref.isLoading())
        return;

    report.kind = findReportKind(kind).value_or(ReportKind::crash);
    report.capturedBy =
        findCaptureSource(capturedBy).value_or(CaptureSource::handler);
    report.attachments.clear();

    for (const auto& attachment: attachments)
        report.attachments.add(FilePath {attachment});
}

std::string CrashReport::toJSON() const
{
    return Miro::toJSONString(*this, reportIndent);
}

std::optional<CrashReport> CrashReport::fromJSON(std::string_view text)
{
    try
    {
        auto report = Miro::createFromJSONString<CrashReport>(text);

        if (report.id.empty())
            return std::nullopt;

        return report;
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
