#pragma once

#include "Connection.h"

namespace eacp::TCP
{

// A bound, listening TCP socket. Move-only RAII, like Connection: holding a
// Listener means it is listening.
class Listener
{
public:
    // Throws TCP::Error on failure. Port 0 asks for an ephemeral port, read
    // back from port(). timeouts.connect bounds accept(); timeouts.io is
    // inherited by every accepted Connection.
    static Listener bind(std::uint16_t port,
                         Timeouts timeouts = {},
                         BindInterface bindTo = BindInterface::loopback);

    ~Listener();

    Listener(Listener&&) noexcept;
    Listener& operator=(Listener&&) noexcept;

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    [[nodiscard]] bool isListening() const;
    void close();

    // The actually-bound port, resolved even when bind(0) was used.
    [[nodiscard]] std::uint16_t port() const;

    // Blocks until a client connects, or throws when the connect timeout
    // elapses.
    Connection accept();

private:
    Listener();

    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::TCP
