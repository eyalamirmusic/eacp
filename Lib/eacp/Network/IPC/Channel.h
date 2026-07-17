#pragma once

#include "Lock.h"

#include <optional>

namespace eacp::IPC
{

// A live byte stream between two of this user's processes on this machine.
//
// One process serves: it claims a name with a ChannelServer and accepts.
// Any other process dials the same name with Channel::connect(). Both ends
// hold a Channel, and a Channel is the same thing on either side: a duplex,
// ordered stream of bytes.
//
// Move-only, like TCP::Connection: holding a Channel means the stream is
// open. The peer vanishing - closed, crashed, killed - surfaces as a clean
// end of stream on receive() and an Error on send, so a dead partner is
// news, not a hang.
//
// One thread may send while another receives; that split is the whole
// concurrency budget. Two senders or two receivers need their own ordering.
class Channel
{
public:
    // Dials the server named name. The server may still be starting up - the
    // usual case right after losing a startup race - so a missing endpoint
    // is retried until timeout elapses; zero or negative asks exactly once.
    // Throws IPC::Error when nobody answered in time.
    static Channel connect(std::string_view name,
                           Time::MS timeout = Time::MS {5000});

    ~Channel();

    Channel(Channel&&) noexcept;
    Channel& operator=(Channel&&) noexcept;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    [[nodiscard]] bool isOpen() const;
    void close();

    // Wakes I/O blocked on this channel in other threads, so a reader
    // thread can be told to leave: an interrupted receive reports a clean
    // end of stream. On POSIX one call is permanent - the socket is shut
    // down. On Windows only calls already in flight are woken, so a
    // teardown loop repeats the interrupt until its reader has actually
    // exited (Messenger's destructor is the reference example). Safe to
    // call from any thread; close() is not, so interrupt-join-close is
    // the teardown order.
    void interrupt();

    // Writes every byte, looping past partial writes. Throws on failure.
    void send(std::string_view bytes);

    // Returns the bytes up to (and consuming) the next delimiter, keeping
    // any overshoot for the following call. Throws if the peer closes
    // before the delimiter arrives.
    std::string receiveUntil(char delimiter);

    // receiveUntil('\n') with a trailing carriage return trimmed.
    std::string receiveLine();

    // Returns whatever a single read yields, up to maxBytes (draining any
    // bytes already buffered by receiveUntil first). An empty string means
    // the peer closed the stream cleanly.
    std::string receive(std::size_t maxBytes = 4096);

    // As above, but into caller-owned storage - no per-read allocation, so
    // a large message can be assembled directly in its final buffer.
    // Returns the byte count; zero means the peer closed the stream cleanly.
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
    // Claims name and starts listening, or throws IPC::Error - most notably
    // when another live server already holds the name. Uniqueness rides on
    // an internal Lock named "<name>.channel": a crashed predecessor's
    // leftovers are swept aside (the kernel released its lock with it),
    // while a live server keeps the name. Names fold to filenames the way
    // Lock names do, and on POSIX they back a socket path with a hard
    // length budget, so keep them short - a bundle id, not a sentence.
    explicit ChannelServer(std::string_view name);

    ~ChannelServer();

    ChannelServer(ChannelServer&&) noexcept;
    ChannelServer& operator=(ChannelServer&&) noexcept;

    ChannelServer(const ChannelServer&) = delete;
    ChannelServer& operator=(const ChannelServer&) = delete;

    [[nodiscard]] bool isListening() const;

    // Retires the endpoint and releases the name for the next server.
    void close();

    // Blocks until a client connects - forever by default, which is what a
    // dedicated acceptor thread wants. A positive timeout bounds the wait
    // and answers nullopt when it elapses with nobody there, so a polling
    // loop can check a stop flag without exceptions as control flow.
    std::optional<Channel> accept(Time::MS timeout = {});

private:
    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::IPC
