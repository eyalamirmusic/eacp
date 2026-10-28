#include "FallbackReport.h"

#include <array>
#include <charconv>
#include <optional>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto fallbackReportCapacity = std::size_t {512 * 1024};
constexpr auto fallbackHexBase = 16;
constexpr auto firstPrintableCharacter = 0x20;
constexpr auto temporarySuffix = ".tmp";

struct PreparedFallbackReport
{
    std::string id;
    std::string platform;
    CrashTimePath folder;
    CrashTimePath temporaryReport;
    CrashTimePath report;
    CrashTimePath crashedLastRunMarker;
};

std::optional<PreparedFallbackReport>& getPreparedReport()
{
    static auto prepared = std::optional<PreparedFallbackReport> {};
    return prepared;
}

// Appends into one fixed buffer and drops whatever does not fit, so a report
// that overflows is cut short and fails to parse rather than overrunning.
class CrashTimeJson
{
public:
    void append(std::string_view text)
    {
        auto count = std::min(text.size(), buffer.size() - length);
        std::copy_n(text.data(), count, buffer.data() + length);
        length += count;
    }

    void appendString(std::string_view text)
    {
        append("\"");

        for (auto character: text)
            appendEscapedCharacter(character);

        append("\"");
    }

    void appendNumber(std::int64_t value)
    {
        auto digits = std::array<char, 24> {};
        auto [end, error] =
            std::to_chars(digits.data(), digits.data() + digits.size(), value);
        append({digits.data(), static_cast<std::size_t>(end - digits.data())});
    }

    void appendHexAddress(std::uint64_t value)
    {
        auto digits = std::array<char, 24> {};
        auto [end, error] = std::to_chars(
            digits.data(), digits.data() + digits.size(), value, fallbackHexBase);
        append("\"0x");
        append({digits.data(), static_cast<std::size_t>(end - digits.data())});
        append("\"");
    }

    void appendKey(std::string_view key)
    {
        appendString(key);
        append(":");
    }

    void reset() { length = 0; }

    std::string_view text() const { return {buffer.data(), length}; }

private:
    void appendEscapedCharacter(char character)
    {
        constexpr auto hexDigits = std::string_view {"0123456789abcdef"};
        constexpr auto nibbleBits = 4;
        constexpr auto nibbleMask = 0x0F;
        auto code = static_cast<unsigned char>(character);

        if (character == '"' || character == '\\')
        {
            append("\\");
            append({&character, 1});
            return;
        }

        if (code >= firstPrintableCharacter)
        {
            append({&character, 1});
            return;
        }

        auto escape = std::array<char, 6> {'\\', 'u', '0', '0', '0', '0'};
        escape[4] = hexDigits[code >> nibbleBits];
        escape[5] = hexDigits[code & nibbleMask];
        append({escape.data(), escape.size()});
    }

    std::array<char, fallbackReportCapacity> buffer {};
    std::size_t length = 0;
};

CrashTimeJson& getCrashTimeJson()
{
    static auto json = CrashTimeJson {};
    return json;
}

void appendFrameJson(CrashTimeJson& json,
                     const SharedCrashInfo& crash,
                     std::uint64_t address)
{
    auto module = findSharedModuleIndex(crash, address);
    auto modulePath = std::string_view {};
    auto moduleBase = std::uint64_t {0};

    if (module)
    {
        modulePath = readBoundedText(crash.modules[*module].path);
        moduleBase = crash.modules[*module].base;
    }

    json.append("{");
    json.appendKey("modulePath");
    json.appendString(modulePath);
    json.append(",");
    json.appendKey("moduleBase");
    json.appendHexAddress(moduleBase);
    json.append(",");
    json.appendKey("offset");
    json.appendHexAddress(address - moduleBase);
    json.append(",");
    json.appendKey("symbol");
    json.appendString({});
    json.append(",");
    json.appendKey("moduleUuid");
    json.appendString({});
    json.append("}");
}

void appendFramesJson(CrashTimeJson& json, const SharedCrashInfo& crash)
{
    auto useThrowSite = crash.throwFrameCount > 0;
    const auto& addresses = useThrowSite ? crash.throwFrames : crash.frames;
    auto count = std::min<std::size_t>(
        useThrowSite ? crash.throwFrameCount : crash.frameCount, maxCrashFrames);

    json.append("[");

    for (auto index = std::size_t {0}; index < count; ++index)
    {
        if (index > 0)
            json.append(",");

        appendFrameJson(json, crash, addresses[index]);
    }

    json.append("]");
}

void appendTableJson(CrashTimeJson& json, const SharedKeyValueTable& table)
{
    auto count = std::min<std::size_t>(table.count, maxMetadataEntries);
    json.append("{");

    for (auto index = std::size_t {0}; index < count; ++index)
    {
        if (index > 0)
            json.append(",");

        json.appendKey(readBoundedText(table.entries[index].key));
        json.appendString(readBoundedText(table.entries[index].value));
    }

    json.append("}");
}

void appendUserJson(CrashTimeJson& json, const SharedUser& user)
{
    auto isSet = user.isSet != 0;
    json.append("{");
    json.appendKey("id");
    json.appendString(isSet ? readBoundedText(user.id) : std::string_view {});
    json.append(",");
    json.appendKey("email");
    json.appendString(isSet ? readBoundedText(user.email) : std::string_view {});
    json.append(",");
    json.appendKey("username");
    json.appendString(isSet ? readBoundedText(user.username) : std::string_view {});
    json.append("}");
}

void appendReportJson(CrashTimeJson& json,
                      const PreparedFallbackReport& prepared,
                      const SharedState& state)
{
    const auto& crash = state.crash;
    json.append("{");
    json.appendKey("id");
    json.appendString(prepared.id);
    json.append(",");
    json.appendKey("kind");
    json.appendString(getReportKindName(ReportKind::crash));
    json.append(",");
    json.appendKey("timestamp");
    json.appendNumber(getMillisecondsSinceEpoch());
    json.append(",");
    json.appendKey("reason");
    json.appendString(readBoundedText(crash.reason));
    json.append(",");
    json.appendKey("message");
    json.appendString(readBoundedText(crash.message));
    json.append(",");
    json.appendKey("faultAddress");
    json.appendHexAddress(crash.faultAddress);
    json.append(",");
    json.appendKey("crashingThread");
    json.append("{");
    json.appendKey("threadId");
    json.appendHexAddress(crash.threadId);
    json.append(",");
    json.appendKey("frames");
    appendFramesJson(json, crash);
    json.append("},");
    json.appendKey("metadata");
    appendTableJson(json, state.metadata);
    json.append(",");
    json.appendKey("tags");
    appendTableJson(json, state.tags);
    json.append(",");
    json.appendKey("extra");
    appendTableJson(json, state.extra);
    json.append(",");
    json.appendKey("user");
    appendUserJson(json, state.user);
    json.append(",");
    json.appendKey("platform");
    json.appendString(prepared.platform);
    json.append(",");
    json.appendKey("capturedBy");
    json.appendString(getCaptureSourceName(CaptureSource::app));
    json.append("}");
}
} // namespace

void prepareFallbackReport(const ReportDatabase& database)
{
    auto id = createReportId();
    auto folder = database.getReportFolder(id);
    auto report = database.getReportFile(id);

    getPreparedReport() = PreparedFallbackReport {
        id,
        getPlatformName(),
        makeCrashTimePath(folder),
        makeCrashTimePath(report.sibling(report.fileName() + temporarySuffix)),
        makeCrashTimePath(report),
        makeCrashTimePath(database.getCrashedLastRunMarker()),
    };
    getCrashTimeJson();
}

bool writeFallbackReport(const SharedState& state)
{
    const auto& prepared = getPreparedReport();

    if (!prepared)
        return false;

    auto& json = getCrashTimeJson();
    json.reset();
    appendReportJson(json, *prepared, state);

    return createCrashTimeDirectory(prepared->folder)
           && writeCrashTimeFile(prepared->temporaryReport, json.text())
           && renameCrashTimeFile(prepared->temporaryReport, prepared->report)
           && writeCrashTimeFile(prepared->crashedLastRunMarker, {});
}
} // namespace eacp::CrashReporting::Detail
