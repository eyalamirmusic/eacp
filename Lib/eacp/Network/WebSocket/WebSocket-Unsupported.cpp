#include "WebSocket.h"

// The WebSocket backend for platforms that do not have one yet.
//
// Apple gets a real implementation from NSURLSessionWebSocketTask. Windows
// and Linux do not, and the obvious shortcut on Linux is a dead end: the
// libcurl that Ubuntu 24.04 ships (8.5.0, what CI runs on) is built without
// the websockets feature, so curl_ws_* compiles against the header and then
// fails at runtime with an unsupported protocol. Rather than ship that, the
// symbols exist here and say so, which keeps every platform linking and
// keeps the failure legible instead of mysterious.
//
// Replacing this means RFC 6455 over TCP::Connection - handshake, masking,
// fragmentation, ping/pong - plus TLS for wss://, which TCP::Connection
// does not have today.

namespace eacp::WebSocket
{
namespace
{
[[noreturn]] void unsupported()
{
    throw Error("WebSocket is not implemented on this platform yet");
}
} // namespace

struct Connection::Impl
{
    std::string empty;
};

Connection::Connection() = default;
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;
Connection::~Connection() = default;

Connection Connection::connect(const std::string& url, Options)
{
    // Parsed first so a caller that got the URL wrong is told that, rather
    // than being told the platform is unsupported and left to discover the
    // second problem after porting.
    parseUrl(url);
    unsupported();
}

bool Connection::isOpen() const
{
    return false;
}

const std::string& Connection::subprotocol() const
{
    unsupported();
}

void Connection::send(const Message&)
{
    unsupported();
}

void Connection::sendText(std::string)
{
    unsupported();
}

void Connection::sendBinary(std::string)
{
    unsupported();
}

std::optional<Message> Connection::receive()
{
    unsupported();
}

void Connection::ping()
{
    unsupported();
}

void Connection::close(int, const std::string&) {}

int Connection::closeCode() const
{
    return 0;
}

const std::string& Connection::closeReason() const
{
    unsupported();
}

} // namespace eacp::WebSocket
