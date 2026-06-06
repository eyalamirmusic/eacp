#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace eacp::TCP
{

// Where to dial. An empty host resolves to the loopback interface.
struct Address
{
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
};

// How long each phase may block before a TimeoutError is thrown. io applies
// to a single send or receive, not to a whole transaction. A zero (or
// negative) duration means "no timeout" - block until it completes, which is
// what a long-lived server wants for accept() and for reads.
struct Timeouts
{
    std::chrono::milliseconds connect {15000};
    std::chrono::milliseconds io {20000};
};

// Every failure - name resolution, a refused connect, a timeout, a peer
// that hangs up mid-read - surfaces as this one exception type, carrying a
// message that is ready to log or show.
struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// A send or receive that ran past its timeout without the peer doing
// anything. Distinct from Error so "the other side went quiet" can be told
// apart from "the connection broke" - handy for idle-driven reads.
struct TimeoutError : Error
{
    using Error::Error;
};

// A live, connected TCP stream.
//
// Move-only by design: if you are holding a Connection, the socket is open.
// There is no half-built state to guard against - dialing happens in
// connect() and either yields an open stream or throws, and the destructor
// closes. Reconnecting means asking connect() for a fresh one.
class Connection
{
public:
    // Opens a stream to address, or throws TCP::Error trying.
    static Connection connect(Address address, Timeouts timeouts = {});

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

    // Wraps an already-connected native socket (an int fd or a SOCKET, passed
    // as intptr_t) in a Connection that owns it. Used by Listener::accept().
    static Connection adopt(std::intptr_t nativeSocket, Address peer);
    friend class Listener;

    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::TCP
