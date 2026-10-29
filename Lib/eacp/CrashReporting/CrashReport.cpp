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

void StackFrame::reflect(Miro::Reflector& ref)
{
    MIRO_FIELDS(ref, modulePath)
    reflectHexAddress(ref, "moduleBase", moduleBase);
    reflectHexAddress(ref, "offset", offset);
    MIRO_FIELDS(ref, symbol, moduleUuid)
}

void CrashedThread::reflect(Miro::Reflector& ref)
{
    reflectHexAddress(ref, "threadId", threadId);
    MIRO_FIELDS(ref, frames)
}

void Breadcrumb::reflect(Miro::Reflector& ref)
{
    auto levelName = std::string {getLevelName(level)};
    MIRO_FIELDS(ref, category, message)
    ref["level"](levelName);
    MIRO_FIELDS(ref, timestamp)

    if (ref.isLoading())
        level = findLevel(levelName).value_or(Level::info);
}

void CrashReport::reflect(Miro::Reflector& ref)
{
    auto kindName = std::string {getReportKindName(kind)};
    auto capturedByName = std::string {getCaptureSourceName(capturedBy)};
    auto attachmentPaths = Vector<std::string> {};

    for (const auto& attachment: attachments)
        attachmentPaths.add(attachment.str());

    MIRO_FIELDS(ref, id)
    ref["kind"](kindName);
    MIRO_FIELDS(ref, timestamp, reason, message)
    reflectHexAddress(ref, "faultAddress", faultAddress);
    MIRO_FIELDS(ref, crashingThread, terminateFrames)
    ref["hangDuration"](hangDurationMilliseconds);
    reflectPath(ref, "minidumpPath", minidumpPath);
    MIRO_FIELDS(ref, metadata, tags, extra, user, breadcrumbs)
    ref["attachments"](attachmentPaths);
    MIRO_FIELDS(ref, platform)
    ref["capturedBy"](capturedByName);

    if (!ref.isLoading())
        return;

    kind = findReportKind(kindName).value_or(ReportKind::crash);
    capturedBy = findCaptureSource(capturedByName).value_or(CaptureSource::handler);
    attachments.clear();

    for (const auto& attachment: attachmentPaths)
        attachments.add(FilePath {attachment});
}

std::string AppSession::toJSON() const
{
    return Miro::toJSONString(*this, reportIndent);
}

std::optional<AppSession> AppSession::fromJSON(std::string_view text)
{
    try
    {
        return Miro::createFromJSONString<AppSession>(text);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
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
