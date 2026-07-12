#pragma once

#include "../Common.h"

namespace eacp::TCP
{

// Where to dial. An empty host resolves to the loopback interface.
struct Address
{
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
};

// How long each phase may block before a TimeoutError. io applies to a single
// send or receive, not a whole transaction. Zero or negative means no timeout
// — what a long-lived server wants for accept() and reads.
struct Timeouts
{
    std::chrono::milliseconds connect {15000};
    std::chrono::milliseconds io {20000};
};

// Every failure - name resolution, refused connect, timeout, peer hangup -
// surfaces as this one exception type, with a message ready to log or show.
struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// A send or receive that ran past its timeout. Distinct from Error so "the
// peer went quiet" can be told apart from "the connection broke".
struct TimeoutError : Error
{
    using Error::Error;
};

// A live, connected TCP stream.
//
// Move-only: holding a Connection means the socket is open. connect() either
// yields an open stream or throws (no half-built state); the destructor
// closes. Reconnect via a fresh connect().
class Connection
{
public:
    // Opens a stream to address, or throws TCP::Error trying.
    static Connection connect(Address address, Timeouts timeouts = {});

    // Wraps an already-connected native socket (an int fd or a SOCKET, passed
    // as intptr_t) in a Connection that owns it. Listener::accept() is the
    // caller that matters; reach for connect() to open a stream yourself.
    static Connection adopt(std::intptr_t nativeSocket, Address peer);

    ~Connection();

    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] bool isOpen() const;
    void close();

    [[nodiscard]] const Address& address() const;

    // Writes every byte, looping past partial writes. Throws on failure.
    void send(std::string_view bytes);

    // Returns the bytes up to (and consuming) the next delimiter, keeping
    // any overshoot for the following call. Throws if the peer closes
    // before the delimiter arrives.
    std::string receiveUntil(char delimiter);

    // receiveUntil('\n') with a trailing carriage return trimmed - the
    // common case for line-oriented protocols.
    std::string receiveLine();

    // Returns whatever a single read yields, up to maxBytes (draining any
    // bytes already buffered by receiveUntil first). An empty string means
    // the peer closed the stream cleanly.
    std::string receive(std::size_t maxBytes = 4096);

private:
    Connection();

    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::TCP
