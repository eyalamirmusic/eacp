#include "WebViewMessages.h"

#include <Miro/Miro.h>

namespace eacp::Graphics::detail
{
namespace
{
// The object key-events.js posts for every key event the page saw. keyCode is
// a Win32 virtual key and modifiers a bitmask (shift 1, ctrl 2, alt 4, meta 8).
struct KeyEventReport
{
    std::string kind;
    bool consumed = false;
    int keyCode = 0;
    int modifiers = 0;
    bool repeat = false;
    std::string key;

    MIRO_REFLECT(kind, consumed, keyCode, modifiers, repeat, key)
};

constexpr auto shiftModifierBit = 0b0001;
constexpr auto controlModifierBit = 0b0010;
constexpr auto altModifierBit = 0b0100;
constexpr auto commandModifierBit = 0b1000;
constexpr auto firstNonAsciiByte = 0x80;

// Typed text only when event.key is a single code point; named keys ("Enter",
// "ArrowUp") are multi-char ASCII and carry no characters, matching the host
// window's WM_CHAR-derived text.
std::string getTypedCharacters(const KeyEventReport& report)
{
    if (report.key.empty())
        return {};

    auto firstByte = static_cast<unsigned char>(report.key[0]);

    if (report.key.size() == 1 || firstByte >= firstNonAsciiByte)
        return report.key;

    return {};
}

// TODO: Miro::tryCreateFromJSONString<KeyEventReport> once eacp's Miro pin
// includes eyalamirmusic/Miro#20.
std::optional<KeyEventReport> readKeyEventReport(const std::string& json)
{
    try
    {
        auto value = Miro::Json::parse(json);
        if (!value.isObject())
            return std::nullopt;

        auto report = KeyEventReport {};
        Miro::fromJSON(report, value);
        return report;
    }
    catch (const Miro::Json::ParseError&)
    {
        return std::nullopt;
    }
}

bool hasKnownKind(const KeyEventReport& report)
{
    return report.kind == "down" || report.kind == "up";
}

ModifierKeys modifiersFromMask(int mask)
{
    return {(mask & shiftModifierBit) != 0,
            (mask & controlModifierBit) != 0,
            (mask & altModifierBit) != 0,
            (mask & commandModifierBit) != 0};
}

std::string readMessageBody(const Miro::Json::Value& field)
{
    if (field.isString())
        return field.asString();

    if (field.isObject() || field.isArray())
        return Miro::Json::print(field);

    return {};
}

std::string readMessageName(const Miro::Json::Object& envelope)
{
    if (auto* field = Miro::Json::find(envelope, "name"); field && field->isString())
        return field->asString();

    return {};
}

WebMessage readWebMessage(const Miro::Json::Value& envelope)
{
    auto message = WebMessage {};

    if (!envelope.isObject())
        return message;

    const auto& object = envelope.asObject();

    message.name = readMessageName(object);

    if (auto* field = Miro::Json::find(object, "body"); field)
        message.body = readMessageBody(*field);

    return message;
}
} // namespace

std::optional<KeyVerdict> parseKeyVerdict(const std::string& json)
{
    auto report = readKeyEventReport(json);
    if (!report || !hasKnownKind(*report))
        return std::nullopt;

    auto verdict = KeyVerdict {};
    verdict.isDown = report->kind == "down";
    verdict.consumed = report->consumed;
    verdict.virtualKey = report->keyCode;
    verdict.modifiers = modifiersFromMask(report->modifiers);
    verdict.isRepeat = report->repeat;
    verdict.characters = getTypedCharacters(*report);
    return verdict;
}

std::optional<WebMessage> parseWebMessageEnvelope(const std::string& json)
{
    try
    {
        return readWebMessage(Miro::Json::parse(json));
    }
    catch (const Miro::Json::ParseError&)
    {
        return std::nullopt;
    }
}

std::string decodeScriptResult(const std::string& resultJson)
{
    try
    {
        auto value = Miro::Json::parse(resultJson);
        return value.isString() ? value.asString() : resultJson;
    }
    catch (const Miro::Json::ParseError&)
    {
        return resultJson;
    }
}

} // namespace eacp::Graphics::detail
