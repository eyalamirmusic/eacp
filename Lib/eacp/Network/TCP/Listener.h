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

    // Binds and listens on port at one IPv4 address of this machine, such as
    // a tailnet address ("100.104.147.35"), or throws TCP::Error, also when
    // ipv4Address does not parse.
    static Listener
        bind(std::uint16_t port, Timeouts timeouts, const std::string& ipv4Address);

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

    // Wakes an accept() blocked in another thread, and every later one, with
    // a TCP::Error - how a server's accept thread is told to leave without
    // waiting out its timeout. Safe from any thread while the listener lives.
    void interrupt();

private:
    Listener();

    static Listener listenOn(std::uint16_t port,
                             Timeouts timeouts,
                             std::uint32_t addressInNetworkOrder);

    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::TCP
