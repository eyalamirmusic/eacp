#pragma once

#include "Channel.h"

namespace eacp::IPC
{

// One live conversation with another of this user's processes, ready for
// app code: where a Channel is a byte stream and a manual reader thread,
// a Messenger is whole messages and main-thread callbacks.
//
// It owns its reader thread, frames every message (length-prefixed, so
// payloads may carry anything, newlines and NULs included) and delivers
// each one on the main thread. send() is safe from any thread. The
// destructor interrupts a blocked read and joins, so teardown needs no
// protocol tricks - no heartbeats, no sentinel messages.
//
// A Messenger lives on the main thread: construct it there, assign the
// callbacks right after construction (nothing is delivered before the
// event loop gets to tick), and destroy it there. Once a callback has been
// wired it may be reassigned freely; deliveries always read the current
// value.
class Messenger
{
public:
    // Dials the server named name in the background, retrying until
    // timeout elapses. onConnected fires when the peer answers;
    // onDisconnected fires instead when nobody did.
    explicit Messenger(std::string_view name, Time::MS timeout = Time::MS {5000});

    // Interrupts the reader, joins it and closes the channel. Pending
    // deliveries that have not run yet are dropped, never fired late.
    ~Messenger();

    Messenger(const Messenger&) = delete;
    Messenger& operator=(const Messenger&) = delete;
    Messenger(Messenger&&) = delete;
    Messenger& operator=(Messenger&&) = delete;

    [[nodiscard]] bool isConnected() const;

    // Hands message to the peer, whole: it arrives as one onMessage there,
    // however large. Callable from any thread. A message sent while not
    // (or no longer) connected is quietly dropped - onDisconnected is the
    // place to learn the conversation is over. Sending blocks only while
    // the peer's buffers are full, so a peer that stops reading stalls
    // its sender; a peer that dies unblocks it.
    void send(const std::string& message);

    // The dial landed (dialing Messengers only: a server-side session
    // arrives in onClient already connected, and this never fires there).
    Callback onConnected = [] {};

    // A whole message from the peer, handed over by value: a receiver that
    // wants to keep the bytes moves them out instead of copying.
    std::function<void(std::string)> onMessage = [](std::string) {};

    // The conversation is over and cannot resume: the peer left, the
    // stream broke, or the dial never landed. Fires at most once.
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

// The serving end: claims a name and turns every client that dials in into
// a live Messenger session.
//
// The server owns its sessions. Each stays valid until its onDisconnected
// has fired - finished sessions are swept when a later client arrives, and
// whatever remains dies with the server. Like Messenger, this is a
// main-thread object.
class MessageServer
{
public:
    // Claims name and starts accepting, or throws IPC::Error - most
    // notably when another live server already holds the name (the
    // ChannelServer rules apply).
    explicit MessageServer(std::string_view name);

    ~MessageServer();

    MessageServer(const MessageServer&) = delete;
    MessageServer& operator=(const MessageServer&) = delete;
    MessageServer(MessageServer&&) = delete;
    MessageServer& operator=(MessageServer&&) = delete;

    // A client dialed in. The session is already connected - wire its
    // onMessage / onDisconnected here; nothing it received can be
    // delivered before this handler returns.
    std::function<void(Messenger&)> onClient = [](Messenger&) {};

private:
    void acceptLoop();

    struct Impl;
    std::shared_ptr<Impl> impl;
};

} // namespace eacp::IPC
