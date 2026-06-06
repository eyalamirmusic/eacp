#include "Connection.h"

#include "ConnectionInternal.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace eacp::TCP
{

// The socket lives in Impl so its destructor is the single place a stream
// gets torn down: ~Connection, move-assignment and close() all funnel here.
struct Connection::Impl
{
    ~Impl() { detail::socketClose(socket); }

    Address address;
    Timeouts timeouts;
    detail::NativeSocket socket = detail::invalidSocket;

    // Bytes already pulled off the socket but not yet handed back - the
    // overshoot from a receiveUntil() read. Drained before touching the wire.
    std::string buffered;
};

Connection::Connection() = default;
Connection::~Connection() = default;
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;

Connection Connection::connect(Address address, Timeouts timeouts)
{
    auto socket = detail::socketConnect(address, timeouts.connect, timeouts.io);

    auto connection = Connection {};
    connection.impl.create();
    connection.impl->address = std::move(address);
    connection.impl->timeouts = timeouts;
    connection.impl->socket = socket;
    return connection;
}

Connection Connection::adopt(std::intptr_t nativeSocket, Address peer)
{
    auto connection = Connection {};
    connection.impl.create();
    connection.impl->address = std::move(peer);
    connection.impl->socket = (detail::NativeSocket) nativeSocket;
    return connection;
}

bool Connection::isOpen() const
{
    return impl && impl->socket != detail::invalidSocket;
}

void Connection::close()
{
    if (!isOpen())
        return;

    detail::socketClose(impl->socket);
    impl->socket = detail::invalidSocket;
}

const Address& Connection::address() const
{
    return impl->address;
}

void Connection::send(std::string_view bytes)
{
    if (!isOpen())
        throw Error("send() on a closed TCP connection");

    auto sent = std::size_t {0};
    while (sent < bytes.size())
        sent += detail::socketSend(
            impl->socket, bytes.data() + sent, bytes.size() - sent);
}

std::string Connection::receiveUntil(char delimiter)
{
    if (!isOpen())
        throw Error("receiveUntil() on a closed TCP connection");

    while (true)
    {
        if (auto at = impl->buffered.find(delimiter); at != std::string::npos)
        {
            auto line = impl->buffered.substr(0, at);
            impl->buffered.erase(0, at + 1);
            return line;
        }

        char chunk[4096];
        auto received = detail::socketReceive(impl->socket, chunk, sizeof(chunk));
        if (received == 0)
            throw Error("peer closed the connection before the delimiter arrived");

        impl->buffered.append(chunk, received);
    }
}

std::string Connection::receiveLine()
{
    auto line = receiveUntil('\n');
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

std::string Connection::receive(std::size_t maxBytes)
{
    if (!isOpen())
        throw Error("receive() on a closed TCP connection");

    if (!impl->buffered.empty())
    {
        auto take = std::min(maxBytes, impl->buffered.size());
        auto out = impl->buffered.substr(0, take);
        impl->buffered.erase(0, take);
        return out;
    }

    auto chunk = std::string(maxBytes, '\0');
    auto received = detail::socketReceive(impl->socket, chunk.data(), chunk.size());
    chunk.resize(received);
    return chunk;
}

} // namespace eacp::TCP
