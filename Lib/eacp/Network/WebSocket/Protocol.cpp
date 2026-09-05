#include "Protocol.h"

#include <array>
#include <random>

namespace eacp::WebSocket::Protocol
{
namespace
{
constexpr auto webSocketHandshakeGuid =
    std::string_view("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

constexpr auto webSocketBase64Alphabet = std::string_view(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");

std::uint32_t webSocketRotateLeft(std::uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

std::uint8_t webSocketByteAt(std::string_view bytes, std::size_t index)
{
    return (std::uint8_t) bytes[index];
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
    auto hash = std::array<std::uint32_t, 5> {
        0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    auto message = webSocketPadForSha1(input);

    for (auto chunk = std::size_t {0}; chunk < message.size(); chunk += 64)
    {
        auto schedule = std::array<std::uint32_t, 80> {};

        for (auto i = std::size_t {0}; i < 16; ++i)
        {
            auto at = chunk + i * 4;
            schedule[i] = ((std::uint32_t) webSocketByteAt(message, at) << 24)
                          | ((std::uint32_t) webSocketByteAt(message, at + 1) << 16)
                          | ((std::uint32_t) webSocketByteAt(message, at + 2) << 8)
                          | (std::uint32_t) webSocketByteAt(message, at + 3);
        }

        for (auto i = std::size_t {16}; i < schedule.size(); ++i)
            schedule[i] =
                webSocketRotateLeft(schedule[i - 3] ^ schedule[i - 8]
                                        ^ schedule[i - 14] ^ schedule[i - 16],
                                    1);

        auto a = hash[0];
        auto b = hash[1];
        auto c = hash[2];
        auto d = hash[3];
        auto e = hash[4];

        for (auto step = 0; step < 80; ++step)
        {
            auto round = webSocketSha1Round(step, b, c, d);
            auto next = webSocketRotateLeft(a, 5) + round.mix + e + round.constant
                        + schedule[(std::size_t) step];

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

std::string webSocketBase64(std::string_view bytes)
{
    auto encoded = std::string();
    encoded.reserve(((bytes.size() + 2) / 3) * 4);

    for (auto i = std::size_t {0}; i < bytes.size(); i += 3)
    {
        auto remaining = bytes.size() - i;
        auto group = (std::uint32_t) webSocketByteAt(bytes, i) << 16;

        if (remaining > 1)
            group |= (std::uint32_t) webSocketByteAt(bytes, i + 1) << 8;

        if (remaining > 2)
            group |= (std::uint32_t) webSocketByteAt(bytes, i + 2);

        encoded.push_back(webSocketBase64Alphabet[(group >> 18) & 0x3F]);
        encoded.push_back(webSocketBase64Alphabet[(group >> 12) & 0x3F]);
        encoded.push_back(
            remaining > 1 ? webSocketBase64Alphabet[(group >> 6) & 0x3F] : '=');
        encoded.push_back(remaining > 2 ? webSocketBase64Alphabet[group & 0x3F]
                                        : '=');
    }

    return encoded;
}

std::array<std::uint8_t, 4> webSocketRandomMask()
{
    static thread_local auto engine = std::mt19937(std::random_device {}());
    auto bytes = std::uniform_int_distribution<int>(0, 255);

    auto key = std::array<std::uint8_t, 4> {};

    for (auto& byte: key)
        byte = (std::uint8_t) bytes(engine);

    return key;
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

std::uint64_t webSocketReadBigEndian(std::string_view buffer,
                                     std::size_t at,
                                     std::size_t bytes)
{
    auto value = std::uint64_t {0};

    for (auto i = std::size_t {0}; i < bytes; ++i)
        value = (value << 8) | webSocketByteAt(buffer, at + i);

    return value;
}

void webSocketAppendLength(std::string& out, std::size_t size, std::uint8_t maskBit)
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
    return webSocketBase64(webSocketSha1(salted));
}

std::string encode(const Frame& frame, bool masked)
{
    auto out = std::string();
    out.reserve(frame.payload.size() + 14);

    out.push_back((char) ((frame.fin ? 0x80 : 0x00) | (std::uint8_t) frame.opcode));

    webSocketAppendLength(out, frame.payload.size(), masked ? 0x80 : 0x00);

    if (!masked)
    {
        out += frame.payload;
        return out;
    }

    auto key = webSocketRandomMask();

    for (auto byte: key)
        out.push_back((char) byte);

    for (auto i = std::size_t {0}; i < frame.payload.size(); ++i)
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
    auto header = std::size_t {2};

    if (lengthCode == 126)
    {
        header = 4;

        if (buffer.size() < header)
            return std::nullopt;

        length = webSocketReadBigEndian(buffer, 2, 2);
    }
    else if (lengthCode == 127)
    {
        header = 10;

        if (buffer.size() < header)
            return std::nullopt;

        length = webSocketReadBigEndian(buffer, 2, 8);

        if ((length >> 63) != 0)
            throw Error("Frame length with its high bit set");
    }

    auto key = std::array<std::uint8_t, 4> {};

    if (masked)
    {
        if (buffer.size() < header + 4)
            return std::nullopt;

        for (auto i = std::size_t {0}; i < key.size(); ++i)
            key[i] = webSocketByteAt(buffer, header + i);

        header += 4;
    }

    if (length > buffer.size() - header)
        return std::nullopt;

    auto decoded = Decoded();
    decoded.consumed = header + (std::size_t) length;
    decoded.frame.opcode = opcode;
    decoded.frame.fin = fin;
    decoded.frame.payload = std::string(buffer.substr(header, (std::size_t) length));

    if (masked)
        for (auto i = std::size_t {0}; i < decoded.frame.payload.size(); ++i)
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

} // namespace eacp::WebSocket::Protocol
