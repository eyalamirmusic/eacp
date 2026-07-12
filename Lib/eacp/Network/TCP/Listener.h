#pragma once

#include "Connection.h"

namespace eacp::TCP
{

// A bound, listening TCP socket - the receiving end.
//
// Move-only RAII, like Connection: if you are holding a Listener it is
// listening. accept() blocks for the next inbound client (up to the connect
// timeout) and hands back a Connection already wired to that peer.
class Listener
{
public:
    // Binds and listens on port, or throws TCP::Error. Port 0 asks the OS for
    // an ephemeral port - read it back from port() afterwards. The Timeouts'
    // connect bounds how long accept() waits; io is inherited by every
    // accepted Connection. Pass BindInterface::any to serve other machines,
    // which a host firewall will want the user to approve.
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

    // Blocks until a client connects (or the connect timeout elapses, which
    // throws), returning the connected stream.
    Connection accept();

private:
    Listener();

    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::TCP
