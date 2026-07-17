#include "ChannelInternal.h"
#include "Names.h"

#include <algorithm>
#include <cstring>

namespace eacp::IPC
{
namespace
{
constexpr auto retryInterval = Time::MS {25};

std::string foldedNonEmpty(std::string_view name)
{
    if (name.empty())
        throw Error("channel name is empty");

    return detail::foldToFileName(name);
}
} // namespace

// The endpoint lives in Impl so its destructor is the single place a stream
// gets torn down: ~Channel, move-assignment and close() all funnel here.
struct Channel::Impl
{
    ~Impl() { detail::channelClose(channel); }

    detail::NativeChannel channel = detail::invalidChannel;

    // Bytes already pulled off the stream but not yet handed back - the
    // overshoot from a receiveUntil() read. Drained before touching the wire.
    std::string buffered;
};

Channel::Channel() = default;
Channel::~Channel() = default;
Channel::Channel(Channel&&) noexcept = default;
Channel& Channel::operator=(Channel&&) noexcept = default;

Channel Channel::connect(std::string_view name, Time::MS timeout)
{
    auto safeName = foldedNonEmpty(name);
    auto deadline = Time::Deadline {timeout};

    for (;;)
    {
        auto handle = detail::channelTryConnect(safeName);

        if (handle != detail::invalidChannel)
        {
            auto channel = Channel {};
            channel.impl.create();
            channel.impl->channel = handle;
            return channel;
        }

        if (deadline.expired())
            throw Error("no server is listening on channel '" + std::string(name)
                        + "'");

        auto remaining = deadline.remaining();
        Time::sleep(remaining < retryInterval ? remaining : retryInterval);
    }
}

bool Channel::isOpen() const
{
    return impl && impl->channel != detail::invalidChannel;
}

void Channel::close()
{
    if (!isOpen())
        return;

    detail::channelClose(impl->channel);
    impl->channel = detail::invalidChannel;
}

void Channel::interrupt()
{
    if (isOpen())
        detail::channelCancel(impl->channel);
}

void Channel::send(std::string_view bytes)
{
    if (!isOpen())
        throw Error("send() on a closed channel");

    auto sent = std::size_t {0};
    while (sent < bytes.size())
        sent += detail::channelSend(
            impl->channel, bytes.data() + sent, bytes.size() - sent);
}

std::string Channel::receiveUntil(char delimiter)
{
    if (!isOpen())
        throw Error("receiveUntil() on a closed channel");

    while (true)
    {
        if (auto at = impl->buffered.find(delimiter); at != std::string::npos)
        {
            auto line = impl->buffered.substr(0, at);
            impl->buffered.erase(0, at + 1);
            return line;
        }

        char chunk[4096];
        auto received = detail::channelReceive(impl->channel, chunk, sizeof(chunk));

        if (received == 0)
            throw Error("peer closed the channel before the delimiter arrived");

        impl->buffered.append(chunk, received);
    }
}

std::string Channel::receiveLine()
{
    auto line = receiveUntil('\n');

    if (!line.empty() && line.back() == '\r')
        line.pop_back();

    return line;
}

std::string Channel::receive(std::size_t maxBytes)
{
    auto chunk = std::string(maxBytes, '\0');
    chunk.resize(receive(chunk.data(), maxBytes));
    return chunk;
}

std::size_t Channel::receive(char* buffer, std::size_t maxBytes)
{
    if (!isOpen())
        throw Error("receive() on a closed channel");

    if (!impl->buffered.empty())
    {
        auto take = std::min(maxBytes, impl->buffered.size());
        std::memcpy(buffer, impl->buffered.data(), take);
        impl->buffered.erase(0, take);
        return take;
    }

    return detail::channelReceive(impl->channel, buffer, maxBytes);
}

// The guard is what makes "one server per name" true: it is taken before
// binding and held until the endpoint is retired, so at any moment at most
// one process may be sweeping and planting the endpoint. A crashed server's
// lock died with it, which is exactly what lets a successor reclaim the
// name without asking anyone.
struct ChannelServer::Impl
{
    ~Impl() { detail::channelServerClose(listener, safeName); }

    std::string safeName;
    std::optional<Lock> lock;
    std::optional<ScopedLock> guard;
    detail::NativeChannel listener = detail::invalidChannel;
};

ChannelServer::ChannelServer(std::string_view name)
{
    impl.create();
    impl->safeName = foldedNonEmpty(name);
    impl->lock.emplace(std::string(name) + ".channel");
    impl->guard.emplace(*impl->lock);

    if (!impl->guard->isLocked())
        throw Error("channel '" + std::string(name) + "' already has a live server");

    impl->listener = detail::channelBind(impl->safeName);
}

ChannelServer::~ChannelServer() = default;
ChannelServer::ChannelServer(ChannelServer&&) noexcept = default;
ChannelServer& ChannelServer::operator=(ChannelServer&&) noexcept = default;

bool ChannelServer::isListening() const
{
    return impl && impl->listener != detail::invalidChannel;
}

void ChannelServer::close()
{
    if (!isListening())
        return;

    detail::channelServerClose(impl->listener, impl->safeName);
    impl->listener = detail::invalidChannel;
    impl->guard.reset();
}

std::optional<Channel> ChannelServer::accept(Time::MS timeout)
{
    if (!isListening())
        throw Error("accept() on a closed channel server");

    auto accepted = detail::channelAccept(impl->listener, impl->safeName, timeout);

    if (accepted == detail::invalidChannel)
        return std::nullopt;

    auto channel = Channel {};
    channel.impl.create();
    channel.impl->channel = accepted;
    return channel;
}

} // namespace eacp::IPC
