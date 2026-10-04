#include "Protocol.h"

#include "../HTTP/HttpProtocol.h"

#include <eacp/Core/Utils/Base64.h>

#include <charconv>
#include <random>

namespace eacp::WebSocket::Protocol
{
namespace
{
constexpr auto webSocketHandshakeGuid =
    std::string_view("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

constexpr auto webSocketSha1Steps = 80;

std::uint32_t webSocketRotateLeft(std::uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

std::uint8_t webSocketByteAt(std::string_view bytes, int index)
{
    return (std::uint8_t) bytes[(std::size_t) index];
}

void webSocketAppendBigEndian(std::string& out, std::uint64_t value, int bytes)
{
    for (auto i = bytes - 1; i >= 0; --i)
        out.push_back((char) ((value >> (8 * i)) & 0xFF));
}

std::string webSocketPadForSha1(std::string_view input)
{
    auto message = std::string(input);
    auto bitLength = (std::uint64_t) input.size() * 8;

    message.push_back((char) 0x80);

    while (message.size() % 64 != 56)
        message.push_back('\0');

    webSocketAppendBigEndian(message, bitLength, 8);
    return message;
}

struct WebSocketSha1Round
{
    std::uint32_t mix = 0;
    std::uint32_t constant = 0;
};

WebSocketSha1Round
    webSocketSha1Round(int step, std::uint32_t b, std::uint32_t c, std::uint32_t d)
{
    if (step < 20)
        return {(b & c) | (~b & d), 0x5A827999};

    if (step < 40)
        return {b ^ c ^ d, 0x6ED9EBA1};

    if (step < 60)
        return {(b & c) | (b & d) | (c & d), 0x8F1BBCDC};

    return {b ^ c ^ d, 0xCA62C1D6};
}

// FIPS 180-4's SHA-1, written here because the accept key is the one digest
// eacp needs and a crypto dependency would cost more than sixty lines.
std::string webSocketSha1(std::string_view input)
{
    auto hash = Array<std::uint32_t, 5> {
        0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    auto message = webSocketPadForSha1(input);

    for (auto chunk = 0; chunk < (int) message.size(); chunk += 64)
    {
        auto schedule = Array<std::uint32_t, webSocketSha1Steps> {};

        for (auto i = 0; i < 16; ++i)
        {
            auto at = chunk + i * 4;
            schedule[i] = ((std::uint32_t) webSocketByteAt(message, at) << 24)
                          | ((std::uint32_t) webSocketByteAt(message, at + 1) << 16)
                          | ((std::uint32_t) webSocketByteAt(message, at + 2) << 8)
                          | (std::uint32_t) webSocketByteAt(message, at + 3);
        }

        for (auto i = 16; i < webSocketSha1Steps; ++i)
            schedule[i] =
                webSocketRotateLeft(schedule[i - 3] ^ schedule[i - 8]
                                        ^ schedule[i - 14] ^ schedule[i - 16],
                                    1);

        auto a = hash[0];
        auto b = hash[1];
        auto c = hash[2];
        auto d = hash[3];
        auto e = hash[4];

        for (auto step = 0; step < webSocketSha1Steps; ++step)
        {
            auto round = webSocketSha1Round(step, b, c, d);
            auto next = webSocketRotateLeft(a, 5) + round.mix + e + round.constant
                        + schedule[step];

            e = d;
            d = c;
            c = webSocketRotateLeft(b, 30);
            b = a;
            a = next;
        }

        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
    }

    auto digest = std::string();

    for (auto word: hash)
        webSocketAppendBigEndian(digest, word, 4);

    return digest;
}

std::uint8_t webSocketRandomByte()
{
    static thread_local auto engine = std::mt19937(std::random_device {}());
    auto bytes = std::uniform_int_distribution<int>(0, 255);

    return (std::uint8_t) bytes(engine);
}

Array<std::uint8_t, 4> webSocketRandomMask()
{
    auto key = Array<std::uint8_t, 4> {};

    for (auto& byte: key)
        byte = webSocketRandomByte();

    return key;
}

int webSocketDefaultPort(bool secure)
{
    return secure ? 443 : 80;
}

std::optional<int> webSocketPortFrom(std::string_view text, bool secure)
{
    if (text.empty())
        return webSocketDefaultPort(secure);

    auto port = 0;
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), port);

    if (error != std::errc() || end != text.data() + text.size() || port < 1
        || port > 65535)
        return std::nullopt;

    return port;
}

struct WebSocketHostAndPort
{
    std::string_view host;
    std::string_view port;
};

std::optional<WebSocketHostAndPort> webSocketSplitAuthority(std::string_view text)
{
    auto at = text.rfind('@');

    if (at != std::string_view::npos)
        text.remove_prefix(at + 1);

    if (!text.starts_with('['))
    {
        auto colon = text.rfind(':');

        if (colon == std::string_view::npos)
            return WebSocketHostAndPort {text, {}};

        return WebSocketHostAndPort {text.substr(0, colon), text.substr(colon + 1)};
    }

    auto close = text.find(']');

    if (close == std::string_view::npos)
        return std::nullopt;

    auto host = text.substr(1, close - 1);
    auto after = text.substr(close + 1);

    if (after.empty())
        return WebSocketHostAndPort {host, {}};

    if (!after.starts_with(':'))
        return std::nullopt;

    return WebSocketHostAndPort {host, after.substr(1)};
}

std::string webSocketHostHeader(const Address& address)
{
    auto host = address.host.find(':') == std::string::npos
                    ? address.host
                    : "[" + address.host + "]";

    if (address.port == webSocketDefaultPort(address.secure))
        return host;

    return host + ":" + std::to_string(address.port);
}

bool webSocketIsHandshakeOwnHeader(std::string_view name)
{
    for (auto owned: {"host",
                      "upgrade",
                      "connection",
                      "sec-websocket-key",
                      "sec-websocket-version",
                      "sec-websocket-protocol"})
        if (Strings::equalsCaseInsensitive(name, owned))
            return true;

    return false;
}

bool webSocketHasLineBreak(std::string_view text)
{
    return text.find_first_of("\r\n") != std::string_view::npos;
}

bool webSocketCanSendHeader(const std::string& name, const std::string& value)
{
    return !name.empty() && !webSocketIsHandshakeOwnHeader(name)
           && !webSocketHasLineBreak(name) && !webSocketHasLineBreak(value);
}

std::string webSocketJoined(const Vector<std::string>& items)
{
    auto joined = std::string();

    for (const auto& item: items)
    {
        if (!joined.empty())
            joined += ", ";

        joined += item;
    }

    return joined;
}

bool webSocketListHasToken(std::string_view list, std::string_view token)
{
    while (!list.empty())
    {
        auto comma = list.find(',');

        if (Strings::equalsCaseInsensitive(Strings::trim(list.substr(0, comma)),
                                           token))
            return true;

        if (comma == std::string_view::npos)
            break;

        list.remove_prefix(comma + 1);
    }

    return false;
}

std::string_view webSocketNextLine(std::string_view& text)
{
    auto end = text.find('\n');
    auto line = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);

    if (line.ends_with('\r'))
        line.remove_suffix(1);

    return line;
}

std::optional<int> webSocketStatusCode(std::string_view statusLine)
{
    if (!statusLine.starts_with("HTTP/"))
        return std::nullopt;

    auto space = statusLine.find(' ');

    if (space == std::string_view::npos)
        return std::nullopt;

    auto codeText = statusLine.substr(space + 1, 3);
    auto code = 0;
    auto [end, error] =
        std::from_chars(codeText.data(), codeText.data() + codeText.size(), code);

    if (error != std::errc() || end != codeText.data() + codeText.size())
        return std::nullopt;

    return code;
}

HandshakeResult webSocketHandshakeFailure(const std::string& error)
{
    auto result = HandshakeResult();
    result.error = error;
    return result;
}

bool webSocketIsControlOpcode(Opcode opcode)
{
    return ((std::uint8_t) opcode & 0x08) != 0;
}

Opcode webSocketOpcodeFrom(std::uint8_t bits)
{
    switch (bits)
    {
        case 0x0:
            return Opcode::continuation;
        case 0x1:
            return Opcode::text;
        case 0x2:
            return Opcode::binary;
        case 0x8:
            return Opcode::close;
        case 0x9:
            return Opcode::ping;
        case 0xA:
            return Opcode::pong;
        default:
            break;
    }

    throw Error("Unknown WebSocket opcode");
}

std::uint64_t webSocketReadBigEndian(std::string_view buffer, int at, int bytes)
{
    auto value = std::uint64_t {0};

    for (auto i = 0; i < bytes; ++i)
        value = (value << 8) | webSocketByteAt(buffer, at + i);

    return value;
}

void webSocketAppendLength(std::string& out, int size, std::uint8_t maskBit)
{
    if (size < 126)
    {
        out.push_back((char) (maskBit | (std::uint8_t) size));
        return;
    }

    if (size <= 0xFFFF)
    {
        out.push_back((char) (maskBit | 126));
        webSocketAppendBigEndian(out, size, 2);
        return;
    }

    out.push_back((char) (maskBit | 127));
    webSocketAppendBigEndian(out, size, 8);
}
} // namespace

std::string acceptKeyFor(std::string_view clientKey)
{
    auto salted = std::string(clientKey) + std::string(webSocketHandshakeGuid);
    return Base64::encode(webSocketSha1(salted));
}

std::string encode(const Frame& frame, bool masked)
{
    auto out = std::string();
    out.reserve(frame.payload.size() + 14);

    out.push_back((char) ((frame.fin ? 0x80 : 0x00) | (std::uint8_t) frame.opcode));

    webSocketAppendLength(out, (int) frame.payload.size(), masked ? 0x80 : 0x00);

    if (!masked)
    {
        out += frame.payload;
        return out;
    }

    auto key = webSocketRandomMask();

    for (auto byte: key)
        out.push_back((char) byte);

    for (auto i = 0; i < (int) frame.payload.size(); ++i)
        out.push_back((char) (webSocketByteAt(frame.payload, i) ^ key[i % 4]));

    return out;
}

std::optional<Decoded> decode(std::string_view buffer)
{
    if (buffer.size() < 2)
        return std::nullopt;

    auto first = webSocketByteAt(buffer, 0);
    auto second = webSocketByteAt(buffer, 1);

    if ((first & 0x70) != 0)
        throw Error("Reserved frame bits are set");

    auto opcode = webSocketOpcodeFrom((std::uint8_t) (first & 0x0F));
    auto fin = (first & 0x80) != 0;
    auto masked = (second & 0x80) != 0;
    auto lengthCode = (std::uint8_t) (second & 0x7F);

    if (webSocketIsControlOpcode(opcode))
    {
        if (!fin)
            throw Error("Fragmented control frame");

        if (lengthCode > 125)
            throw Error("Control frame longer than 125 bytes");
    }

    auto length = (std::uint64_t) lengthCode;
    auto header = 2;

    if (lengthCode == 126)
    {
        header = 4;

        if (buffer.size() < (std::size_t) header)
            return std::nullopt;

        length = webSocketReadBigEndian(buffer, 2, 2);
    }
    else if (lengthCode == 127)
    {
        header = 10;

        if (buffer.size() < (std::size_t) header)
            return std::nullopt;

        length = webSocketReadBigEndian(buffer, 2, 8);

        if ((length >> 63) != 0)
            throw Error("Frame length with its high bit set");
    }

    auto key = Array<std::uint8_t, 4> {};

    if (masked)
    {
        if (buffer.size() < (std::size_t) header + 4)
            return std::nullopt;

        for (auto& byte: key)
            byte = webSocketByteAt(buffer, header++);
    }

    if (length > buffer.size() - (std::size_t) header)
        return std::nullopt;

    auto decoded = Decoded();
    decoded.consumed = header + (int) length;
    decoded.frame.opcode = opcode;
    decoded.frame.fin = fin;
    decoded.frame.payload =
        std::string(buffer.substr((std::size_t) header, (std::size_t) length));

    if (masked)
        for (auto i = 0; i < (int) decoded.frame.payload.size(); ++i)
            decoded.frame.payload[i] =
                (char) (webSocketByteAt(decoded.frame.payload, i) ^ key[i % 4]);

    return decoded;
}

std::string encodeClose(int code, std::string_view reason)
{
    // 1005 is what an empty payload reads back as, so it goes out as the empty
    // payload it came from rather than as a code no peer may put on the wire.
    if (code == 1005)
        return {};

    auto payload = std::string();
    webSocketAppendBigEndian(payload, (std::uint64_t) (std::uint16_t) code, 2);
    payload += reason;
    return payload;
}

CloseStatus decodeClose(std::string_view payload)
{
    if (payload.empty())
        return {};

    if (payload.size() == 1)
        throw Error("Close payload of a single byte");

    auto status = CloseStatus();
    status.code = (int) webSocketReadBigEndian(payload, 0, 2);
    status.reason = std::string(payload.substr(2));
    return status;
}

std::optional<Address> parseUrl(std::string_view url)
{
    auto schemeEnd = url.find("://");

    if (schemeEnd == std::string_view::npos)
        return std::nullopt;

    auto address = Address();
    auto scheme = Strings::toLower(url.substr(0, schemeEnd));

    if (scheme == "wss")
        address.secure = true;
    else if (scheme != "ws")
        return std::nullopt;

    auto rest = url.substr(schemeEnd + 3);
    rest = rest.substr(0, rest.find('#'));

    auto authorityEnd = rest.find_first_of("/?");
    auto authority = webSocketSplitAuthority(rest.substr(0, authorityEnd));

    if (!authority.has_value() || authority->host.empty())
        return std::nullopt;

    auto port = webSocketPortFrom(authority->port, address.secure);

    if (!port.has_value())
        return std::nullopt;

    address.host = std::string(authority->host);
    address.port = *port;

    if (authorityEnd != std::string_view::npos)
    {
        auto target = rest.substr(authorityEnd);
        address.target = target.starts_with('?') ? "/" + std::string(target)
                                                 : std::string(target);
    }

    return address;
}

std::string randomClientKey()
{
    auto bytes = std::string(16, '\0');

    for (auto& byte: bytes)
        byte = (char) webSocketRandomByte();

    return Base64::encode(bytes);
}

std::string clientHandshakeRequest(const Address& address,
                                   std::string_view key,
                                   const Options& options)
{
    auto request = "GET " + address.target + " HTTP/1.1\r\n";
    request += "Host: " + webSocketHostHeader(address) + "\r\n";
    request += "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Version: 13\r\n";
    request += "Sec-WebSocket-Key: " + std::string(key) + "\r\n";

    if (!options.protocols.empty())
        request +=
            "Sec-WebSocket-Protocol: " + webSocketJoined(options.protocols) + "\r\n";

    for (const auto& [name, value]: options.headers)
        if (webSocketCanSendHeader(name, value))
            request.append(name).append(": ").append(value).append("\r\n");

    return request + "\r\n";
}

HandshakeResult validateHandshakeResponse(std::string_view head,
                                          std::string_view key,
                                          const Vector<std::string>& offered)
{
    auto status = webSocketStatusCode(webSocketNextLine(head));

    if (!status.has_value())
        return webSocketHandshakeFailure("The server's answer is not HTTP");

    if (*status != 101)
        return webSocketHandshakeFailure("The server answered the upgrade with "
                                         + std::to_string(*status));

    auto headers = std::map<std::string, std::string>();

    while (!head.empty())
        HTTP::addHeaderLine(webSocketNextLine(head), headers);

    auto header = [&headers](const std::string& name)
    { return HTTP::findHeaderIgnoringCase(headers, name); };

    if (!Strings::equalsCaseInsensitive(header("Upgrade"), "websocket"))
        return webSocketHandshakeFailure("The server did not upgrade to websocket");

    if (!webSocketListHasToken(header("Connection"), "upgrade"))
        return webSocketHandshakeFailure("The server's Connection is not Upgrade");

    if (header("Sec-WebSocket-Accept") != acceptKeyFor(key))
        return webSocketHandshakeFailure(
            "The server's Sec-WebSocket-Accept is wrong");

    auto result = HandshakeResult();
    result.protocol = header("Sec-WebSocket-Protocol");

    if (!result.protocol.empty() && !offered.contains(result.protocol))
        return webSocketHandshakeFailure(
            "The server chose a subprotocol not offered: " + result.protocol);

    result.ok = true;
    return result;
}

} // namespace eacp::WebSocket::Protocol
