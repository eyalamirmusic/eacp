#pragma once

#include "Lock.h"

#include <optional>

namespace eacp::IPC
{

// A move-only duplex byte stream between two of this user's processes. A
// vanished peer surfaces as end of stream on receive() and an Error on send.
// One thread may send while another receives; more need their own ordering.
class Channel
{
public:
    // Blocks, retrying a missing endpoint until timeout elapses; zero or
    // negative asks exactly once. Throws IPC::Error when nobody answered.
    static Channel connect(std::string_view name,
                           Time::MS timeout = Time::MS {5000});

    ~Channel();

    Channel(Channel&&) noexcept;
    Channel& operator=(Channel&&) noexcept;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    [[nodiscard]] bool isOpen() const;
    void close();

    // Wakes I/O blocked in other threads; an interrupted receive reports a
    // clean end of stream. Permanent on POSIX, one-shot on Windows, so a
    // teardown repeats it. Callable from any thread, unlike close().
    void interrupt();

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

    // As above, into caller-owned storage. Zero means the peer closed cleanly.
    std::size_t receive(char* buffer, std::size_t maxBytes);

private:
    friend class ChannelServer;

    Channel();

    struct Impl;
    OwningPointer<Impl> impl;
};

// The serving end of a named channel: claims the name, then accepts one
// Channel per client that dials it.
class ChannelServer
{
public:
    // Throws IPC::Error when another live server holds the name; a crashed
    // predecessor's leftovers are swept aside. On POSIX the name backs a
    // socket path with a hard length budget, so keep it short.
    explicit ChannelServer(std::string_view name);

    ~ChannelServer();

    ChannelServer(ChannelServer&&) noexcept;
    ChannelServer& operator=(ChannelServer&&) noexcept;

    ChannelServer(const ChannelServer&) = delete;
    ChannelServer& operator=(const ChannelServer&) = delete;

    [[nodiscard]] bool isListening() const;

    // Retires the endpoint and releases the name for the next server.
    void close();

    // Blocks until a client connects, forever by default. A positive timeout
    // bounds the wait and answers nullopt when it elapses with nobody there.
    std::optional<Channel> accept(Time::MS timeout = {});

private:
    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::IPC
