#include "Listener.h"

#include "ConnectionInternal.h"

#include <memory>

namespace eacp::TCP
{

struct Listener::Impl
{
    ~Impl() { detail::socketClose(socket); }

    Timeouts timeouts;
    detail::NativeSocket socket = detail::invalidSocket;
    std::uint16_t port = 0;
};

Listener::Listener() = default;
Listener::~Listener() = default;
Listener::Listener(Listener&&) noexcept = default;
Listener& Listener::operator=(Listener&&) noexcept = default;

Listener Listener::bind(std::uint16_t port, Timeouts timeouts)
{
    auto boundPort = port;
    auto socket = detail::socketListen(port, boundPort);

    auto listener = Listener {};
    listener.impl.create();
    listener.impl->timeouts = timeouts;
    listener.impl->socket = socket;
    listener.impl->port = boundPort;
    return listener;
}

bool Listener::isListening() const
{
    return impl && impl->socket != detail::invalidSocket;
}

void Listener::close()
{
    if (!isListening())
        return;

    detail::socketClose(impl->socket);
    impl->socket = detail::invalidSocket;
}

std::uint16_t Listener::port() const
{
    return impl ? impl->port : 0;
}

Connection Listener::accept()
{
    if (!isListening())
        throw Error("accept() on a closed TCP listener");

    auto peer = Address {};
    auto socket = detail::socketAccept(
        impl->socket, impl->timeouts.connect, impl->timeouts.io, peer);
    return Connection::adopt((std::intptr_t) socket, std::move(peer));
}

} // namespace eacp::TCP
