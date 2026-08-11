#pragma once

#include "Channel.h"

namespace eacp::IPC
{

// Whole length-prefixed messages over a Channel, so payloads may carry NULs.
// Owns its reader thread; construct, wire the callbacks, and destroy it on the
// main thread, where every callback is delivered. send() is thread-safe.
class Messenger
{
public:
    // Dials name in the background, retrying until timeout elapses.
    // onConnected fires when the peer answers, onDisconnected when nobody did.
    explicit Messenger(std::string_view name, Time::MS timeout = Time::MS {5000});

    // Interrupts the reader, joins it and closes the channel. Pending
    // deliveries are dropped, never fired late.
    ~Messenger();

    Messenger(const Messenger&) = delete;
    Messenger& operator=(const Messenger&) = delete;
    Messenger(Messenger&&) = delete;
    Messenger& operator=(Messenger&&) = delete;

    [[nodiscard]] bool isConnected() const;

    // Arrives as one onMessage on the peer, however large. Callable from any
    // thread; drops quietly when not connected. Blocks only while the peer's
    // buffers are full, so a peer that stops reading stalls its sender.
    void send(const std::string& message);

    // Dialing Messengers only: a server-side session arrives in onClient
    // already connected.
    Callback onConnected = [] {};

    // By value, so a receiver keeping the bytes moves them out.
    std::function<void(std::string)> onMessage = [](std::string) {};

    // The peer left, the stream broke, or the dial never landed. Fires at
    // most once.
    Callback onDisconnected = [] {};

private:
    friend class MessageServer;

    explicit Messenger(Channel connectedChannel);

    void begin();
    [[nodiscard]] bool finished() const;

    void work();
    bool dial();
    void readUntilGone();
    void notifyMain(Callback callback);

    struct Impl;
    std::shared_ptr<Impl> impl;
};

// Turns every client that dials in into a live Messenger session. Owns those
// sessions: each stays valid until its onDisconnected has fired, and whatever
// remains dies with the server. A main-thread object, like Messenger.
class MessageServer
{
public:
    // Throws IPC::Error when another live server already holds the name (the
    // ChannelServer rules apply).
    explicit MessageServer(std::string_view name);

    ~MessageServer();

    MessageServer(const MessageServer&) = delete;
    MessageServer& operator=(const MessageServer&) = delete;
    MessageServer(MessageServer&&) = delete;
    MessageServer& operator=(MessageServer&&) = delete;

    // Wire the session's onMessage / onDisconnected here: nothing it received
    // can be delivered before this handler returns.
    std::function<void(Messenger&)> onClient = [](Messenger&) {};

private:
    void acceptLoop();

    struct Impl;
    std::shared_ptr<Impl> impl;
};

} // namespace eacp::IPC
