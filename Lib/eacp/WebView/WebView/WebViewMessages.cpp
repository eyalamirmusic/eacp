#include "WebViewMessages.h"

#include <Miro/Miro.h>

namespace eacp::Graphics::detail
{
namespace
{
constexpr auto shiftModifierBit = 1;
constexpr auto controlModifierBit = 2;
constexpr auto altModifierBit = 4;
constexpr auto commandModifierBit = 8;
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

bool isKnownKeyEventKind(const std::string& kind)
{
    return kind == "down" || kind == "up";
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
    if (!report || !isKnownKeyEventKind(report->kind))
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

std::optional<std::string> getUrlScheme(const std::string& url)
{
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
        return std::nullopt;

    return url.substr(0, schemeEnd);
}

std::string getStreamingStatusReason(int statusCode)
{
    switch (statusCode)
    {
        case 206:
            return "Partial Content";
        case 416:
            return "Range Not Satisfiable";
        default:
            return "OK";
    }
}

std::string getResourceStatusReason(int statusCode)
{
    constexpr auto firstSuccessStatus = 200;
    constexpr auto firstRedirectStatus = 300;

    if (statusCode >= firstSuccessStatus && statusCode < firstRedirectStatus)
        return "OK";

    return "Error";
}

std::string
    joinResponseHeaders(const Vector<std::pair<std::string, std::string>>& headers)
{
    auto joined = std::string {};

    for (const auto& [name, value]: headers)
    {
        if (!joined.empty())
            joined += "\r\n";
        joined += name + ": " + value;
    }

    return joined;
}

std::string buildMessageHandlerScript(const std::string& name)
{
    return "(function(){var send=function(msg){window.chrome.webview.postMessage("
           "{name:'"
           + name
           + "',body:msg});};"
             "window."
           + name
           + "={postMessage:send};"
             "window.webkit=window.webkit||{};"
             "window.webkit.messageHandlers=window.webkit.messageHandlers||{};"
             "window.webkit.messageHandlers."
           + name + "={postMessage:send};})();";
}

std::string buildMaximizedStateScript(bool isMaximized)
{
    return isMaximized ? "window.__eacpSetMaximized(true)"
                       : "window.__eacpSetMaximized(false)";
}
} // namespace eacp::Graphics::detail
