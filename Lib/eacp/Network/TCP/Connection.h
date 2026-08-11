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
    Time::MS connect {15000};
    Time::MS io {20000};
};

struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Distinct from Error so "the peer went quiet" is distinguishable from "the
// connection broke".
struct TimeoutError : Error
{
    using Error::Error;
};

// A live, connected TCP stream. Move-only: holding a Connection means the
// socket is open, and the destructor closes it.
class Connection
{
public:
    // Blocks; throws TCP::Error rather than yielding a half-built stream.
    static Connection connect(Address address, Timeouts timeouts = {});

    // Takes ownership of an already-connected native socket (an int fd or a
    // SOCKET, passed as intptr_t).
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

    // Blocks for the bytes up to (and consuming) the next delimiter, keeping
    // any overshoot for the following call. Throws if the peer closes first.
    std::string receiveUntil(char delimiter);

    // receiveUntil('\n') with a trailing carriage return trimmed.
    std::string receiveLine();

    // Whatever a single read yields, up to maxBytes, draining the
    // receiveUntil overshoot first. Empty means the peer closed cleanly.
    std::string receive(std::size_t maxBytes = 4096);

private:
    Connection();

    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::TCP
