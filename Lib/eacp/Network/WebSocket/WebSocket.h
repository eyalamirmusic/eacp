#pragma once

#include "../Common.h"

#include <map>
#include <optional>

namespace eacp::WebSocket
{

// Which kind of frame carried a payload. This is part of the wire contract
// rather than a detail: a peer expecting JSON text rejects the same bytes
// sent as binary, and audio sent as text is corrupted by UTF-8 validation.
enum class MessageType
{
    text,
    binary
};

struct Message
{
    std::string data;
    MessageType type = MessageType::text;

    [[nodiscard]] bool isBinary() const { return type == MessageType::binary; }

    static Message text(std::string payload);
    static Message binary(std::string payload);
};

// The RFC 6455 codes a caller actually branches on. The protocol reserves
// the rest of the range, and a peer may send any of them, so closeCode()
// returns the raw number - these name the ones worth comparing against.
namespace CloseCode
{
constexpr int normal = 1000;
constexpr int goingAway = 1001;
constexpr int protocolError = 1002;
constexpr int unsupportedData = 1003;
constexpr int noStatus = 1005;
constexpr int abnormal = 1006;
constexpr int messageTooBig = 1009;
constexpr int internalError = 1011;
} // namespace CloseCode

// How long each phase may block before a TimeoutError. io applies to a
// single send or receive, not the session. Zero or negative means no
// timeout - what a long-lived subscription wants for receive().
struct Timeouts
{
    Time::MS connect {15000};
    Time::MS io {0};
};

struct Options
{
    // Sent on the opening handshake. This is the only place a service's
    // credential can go: the WebSocket URL is not a place for a secret,
    // since it lands in proxy and server logs the way a query string does.
    std::map<std::string, std::string> headers;

    // Offered in Sec-WebSocket-Protocol. Empty means no preference.
    Vector<std::string> subprotocols;

    Timeouts timeouts;

    // Refuses a frame larger than this rather than growing to meet it, so a
    // buggy or hostile peer cannot drive the process out of memory.
    std::size_t maxMessageBytes = 16 * 1024 * 1024;
};

// Every failure - bad URL, refused handshake, TLS rejection, timeout, peer
// hangup mid-frame - surfaces as this one type, with a message ready to log.
struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// A send or receive that ran past its timeout. Distinct from Error so "the
// peer went quiet" can be told apart from "the connection broke", which is
// the difference between waiting longer and reconnecting.
struct TimeoutError : Error
{
    using Error::Error;
};

// A live WebSocket session.
//
// Move-only: holding a Connection means the socket is open. connect() either
// yields an open session or throws (no half-built state); the destructor
// closes. Reconnect via a fresh connect().
//
// Blocking, like TCP::Connection - the caller decides which thread waits.
// Run receive() on its own thread and hand results to the event loop with
// Threads::callAsync when the result belongs on the main thread.
class Connection
{
public:
    // Opens a session to url (ws:// or wss://), or throws Error trying.
    // Returns only after the handshake completes, so an open Connection has
    // already been accepted by the server.
    static Connection connect(const std::string& url, Options options = {});

    ~Connection();

    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] bool isOpen() const;

    // The subprotocol the server selected, empty if it chose none.
    [[nodiscard]] const std::string& subprotocol() const;

    void send(const Message& message);
    void sendText(std::string payload);
    void sendBinary(std::string payload);

    // Blocks for the next message. An empty result means the peer closed the
    // session cleanly - read closeCode() and closeReason() for why - which
    // makes the receive loop `while (auto m = ws.receive())` rather than an
    // exception handler around the expected end of a stream.
    std::optional<Message> receive();

    // Round-trips a ping frame to keep an idle session alive, since a
    // service that sees no traffic will eventually drop it. Throws on a
    // dead connection, which is what makes this a liveness check.
    void ping();

    // Sends a close frame and waits for the peer's. Safe to call twice; the
    // destructor calls it for you.
    void close(int code = CloseCode::normal, const std::string& reason = {});

    // Why the session ended. Only meaningful once receive() has reported the
    // close or close() has run; before that the code is zero.
    [[nodiscard]] int closeCode() const;
    [[nodiscard]] const std::string& closeReason() const;

private:
    Connection();

    struct Impl;
    OwningPointer<Impl> impl;
};

// Where a ws:// or wss:// URL points. Split out from connect() because every
// backend needs the same decomposition, and because getting it wrong is
// silent - a mis-parsed path reaches a real server that answers 404.
struct Url
{
    bool secure = false;
    std::string host;
    std::uint16_t port = 0;
    std::string target = "/";
};

// Throws Error on anything that is not a well-formed ws:// or wss:// URL.
Url parseUrl(const std::string& url);

} // namespace eacp::WebSocket
