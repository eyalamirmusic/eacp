#include "WebViewMessages.h"

#include <Miro/Miro.h>

#include <cstdlib>
#include <string_view>

namespace eacp::Graphics::detail
{
namespace
{
constexpr auto shiftModifierBit = 1;
constexpr auto controlModifierBit = 2;
constexpr auto altModifierBit = 4;
constexpr auto commandModifierBit = 8;
constexpr auto firstNonAsciiByte = 0x80;

class ColonFieldReader
{
public:
    explicit ColonFieldReader(std::string_view messageToUse)
        : message(messageToUse)
    {
    }

    std::string readField()
    {
        if (isFinished())
            return {};

        auto colon = message.find(':', position);

        if (colon == std::string::npos)
            return takeRemainder();

        auto field = std::string {message.substr(position, colon - position)};
        position = colon + 1;
        return field;
    }

    std::string takeRemainder()
    {
        if (isFinished())
            return {};

        auto remainder = std::string {message.substr(position)};
        position = std::string::npos;
        return remainder;
    }

    int readIntegerField()
    {
        auto field = readField();
        return field.empty() ? 0 : std::atoi(field.c_str());
    }

    bool readFlagField() { return readField() == "1"; }

    // The remainder as typed text: only a single code point counts; named keys
    // ("Enter", "ArrowUp") are multi-char ASCII and carry no characters,
    // matching the host window's WM_CHAR-derived text.
    std::string takeTypedCharacters()
    {
        auto key = takeRemainder();

        if (key.empty())
            return {};

        auto firstByte = static_cast<unsigned char>(key[0]);

        if (key.size() == 1 || firstByte >= firstNonAsciiByte)
            return key;

        return {};
    }

private:
    bool isFinished() const { return position == std::string::npos; }

    std::string_view message;
    std::size_t position = 0;
};

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

std::optional<KeyVerdict> parseKeyVerdict(const std::string& message)
{
    auto reader = ColonFieldReader {message};

    auto kind = reader.readField();
    if (kind != "down" && kind != "up")
        return std::nullopt;

    auto verdict = KeyVerdict {};
    verdict.isDown = kind == "down";
    verdict.consumed = reader.readFlagField();
    verdict.virtualKey = reader.readIntegerField();
    verdict.modifiers = modifiersFromMask(reader.readIntegerField());
    verdict.isRepeat = reader.readFlagField();

    verdict.characters = reader.takeTypedCharacters();

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
