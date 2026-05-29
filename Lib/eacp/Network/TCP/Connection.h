#pragma once

#include <ea_data_structures/Pointers/OwningPointer.h>

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

// How long each phase may block before a TCP::Error is thrown. io applies
// to a single send or receive, not to a whole transaction.
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

    struct Impl;
    EA::OwningPointer<Impl> impl;
};

} // namespace eacp::TCP
