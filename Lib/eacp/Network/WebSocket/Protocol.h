#pragma once

#include "WebSocket.h"

#include <optional>

// RFC 6455's framing and handshake arithmetic - what a server speaks
namespace eacp::WebSocket::Protocol
{

enum class Opcode : std::uint8_t
{
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

struct Frame
{
    Opcode opcode = Opcode::text;
    bool fin = true;
    std::string payload;
};

struct Decoded
{
    Frame frame;
    int consumed = 0;
};

struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// §4.2.2: base64(SHA-1(key + GUID))
std::string acceptKeyFor(std::string_view clientKey);

// §5.2. A masked frame is what a client sends; the mask is random.
std::string encode(const Frame& frame, bool masked = false);

// One frame off the front of buffer, nullopt while bytes are still missing;
// throws Error on a header no peer may send.
std::optional<Decoded> decode(std::string_view buffer);

// §5.5.1's close payload, both ways; an empty payload reads as 1005
std::string encodeClose(int code, std::string_view reason);
CloseStatus decodeClose(std::string_view payload);

// A ws:// or wss:// URL taken apart. The host is bare, an IPv6 literal without
// its brackets; target is the path and query, "/" when the URL has neither.
struct Address
{
    bool secure = false;
    std::string host;
    int port = 0;
    std::string target = "/";
};

std::optional<Address> parseUrl(std::string_view url);

// §4.1: sixteen random bytes, base64
std::string randomClientKey();

// The client's upgrade request, header block and blank line included
std::string clientHandshakeRequest(const Address& address,
                                   std::string_view key,
                                   const Options& options);

struct HandshakeResult
{
    bool ok = false;
    std::string error;
    std::string protocol;
};

// §4.1's checks on the server's answer, given its header block - the status
// line through the blank line, or without it.
HandshakeResult validateHandshakeResponse(std::string_view head,
                                          std::string_view key,
                                          const Vector<std::string>& offered);

} // namespace eacp::WebSocket::Protocol
