#include "Messenger.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <thread>

namespace eacp::IPC
{
namespace
{
// The slices are what keep every blocking wait interruptible: a worker
// checks its stop flag between slices, so no teardown waits longer than
// one of these.
constexpr auto dialSlice = Time::MS {200};
constexpr auto acceptSlice = Time::MS {200};
constexpr auto interruptRetry = Time::MS {5};

// A frame is a 32-bit little-endian byte count, then that many bytes. The
// prefix is what frees payloads to carry anything - newlines, NULs, whole
// files.
constexpr auto headerSize = std::size_t {4};

// Refuses lengths no honest peer would send, so a framing bug surfaces as
// an error instead of a gigabyte allocation.
constexpr auto maxMessageSize = std::size_t {1} << 30;

std::array<char, headerSize> encodeHeader(std::size_t size)
{
    auto header = std::array<char, headerSize> {};

    for (auto index = std::size_t {0}; index < headerSize; ++index)
        header[index] = (char) ((size >> (index * 8)) & 0xff);

    return header;
}

std::size_t decodeLength(const std::string& header)
{
    auto size = std::uint32_t {0};

    for (auto index = std::size_t {0}; index < headerSize; ++index)
        size |= (std::uint32_t) (unsigned char) header[index] << (index * 8);

    return size;
}

// Fills result with exactly count bytes, sized once and written in place;
// false means the stream ended first, leaving what did arrive in result.
bool receiveExactly(Channel& channel, std::size_t count, std::string& result)
{
    result.resize(count);
    auto received = std::size_t {0};

    while (received < count)
    {
        auto chunk = channel.receive(result.data() + received, count - received);

        if (chunk == 0)
        {
            result.resize(received);
            return false;
        }

        received += chunk;
    }

    return true;
}

// One whole message, or nullopt when the stream ended cleanly between
// frames. A stream that ends mid-frame throws, like any other broken read.
std::optional<std::string> receiveFrame(Channel& channel)
{
    auto header = std::string {};

    if (!receiveExactly(channel, headerSize, header))
    {
        if (header.empty())
            return std::nullopt;

        throw Error("peer closed the channel mid-message");
    }

    auto length = decodeLength(header);

    if (length > maxMessageSize)
        throw Error("peer sent an implausible message length");

    auto message = std::string {};

    if (!receiveExactly(channel, length, message))
        throw Error("peer closed the channel mid-message");

    return message;
}
} // namespace

// Shared with every delivery queued onto the main thread: a delivery that
// fires after the Messenger died still finds the stop flag (and backs off)
// instead of dangling. The mutex guards the channel's existence for
// senders; the reader uses it without locking, which the Channel's
// one-sender-one-receiver contract allows.
struct Messenger::Impl
{
    std::string name;
    Time::MS timeout {};

    std::mutex mutex;
    std::optional<Channel> channel;

    std::atomic<bool> stop = false;
    std::atomic<bool> workerDone = false;
    std::thread worker;
};

Messenger::Messenger(std::string_view name, Time::MS timeout)
    : impl(std::make_shared<Impl>())
{
    impl->name = std::string {name};
    impl->timeout = timeout;
    begin();
}

Messenger::Messenger(Channel connectedChannel)
    : impl(std::make_shared<Impl>())
{
    impl->channel.emplace(std::move(connectedChannel));
}

Messenger::~Messenger()
{
    impl->stop = true;

    // Interrupt until the reader acknowledges: on POSIX the first pass
    // wakes it for good; on Windows a cancel only reaches a read already
    // in flight, so keep knocking (see Channel::interrupt).
    while (impl->worker.joinable() && !impl->workerDone)
    {
        {
            auto guard = std::lock_guard {impl->mutex};

            if (impl->channel)
                impl->channel->interrupt();
        }

        Time::sleep(interruptRetry);
    }

    if (impl->worker.joinable())
        impl->worker.join();
}

void Messenger::begin()
{
    impl->worker = std::thread([this] { work(); });
}

bool Messenger::finished() const
{
    return impl->workerDone;
}

bool Messenger::isConnected() const
{
    auto guard = std::lock_guard {impl->mutex};
    return impl->channel && impl->channel->isOpen();
}

void Messenger::send(const std::string& message)
{
    if (message.size() > maxMessageSize)
        throw Error("message is too large to frame");

    auto guard = std::lock_guard {impl->mutex};

    if (!impl->channel || !impl->channel->isOpen())
        return;

    try
    {
        // Header and payload go out as two writes so framing never copies
        // the payload - the mutex keeps them adjacent on the stream.
        auto header = encodeHeader(message.size());
        impl->channel->send({header.data(), header.size()});
        impl->channel->send(message);
    }
    catch (const Error&)
    {
        // The reader is about to hear the same news and report it.
    }
}

void Messenger::work()
{
    auto dialed = !impl->name.empty();

    if (dialed && !dial())
    {
        if (!impl->stop)
            notifyMain([this] { onDisconnected(); });

        impl->workerDone = true;
        return;
    }

    if (dialed)
        notifyMain([this] { onConnected(); });

    readUntilGone();
    impl->workerDone = true;
}

bool Messenger::dial()
{
    auto deadline = Time::Deadline {impl->timeout};

    while (!impl->stop)
    {
        auto remaining = deadline.remaining();

        try
        {
            auto opened = Channel::connect(
                impl->name, remaining < dialSlice ? remaining : dialSlice);

            auto guard = std::lock_guard {impl->mutex};
            impl->channel.emplace(std::move(opened));
            return true;
        }
        catch (const Error&)
        {
            if (deadline.expired())
                return false;
        }
    }

    return false;
}

void Messenger::readUntilGone()
{
    try
    {
        for (;;)
        {
            auto message = receiveFrame(*impl->channel);

            // An interrupted read surfaces as end of stream; the flag is
            // what tells a teardown apart from the peer leaving.
            if (impl->stop)
                return;

            if (!message)
                break;

            notifyMain([this, delivered = std::move(*message)]() mutable
                       { onMessage(std::move(delivered)); });
        }
    }
    catch (const Error&)
    {
        // A broken stream ends the conversation the same way a clean
        // close does.
    }

    if (impl->stop)
        return;

    {
        auto guard = std::lock_guard {impl->mutex};
        impl->channel.reset();
    }

    notifyMain([this] { onDisconnected(); });
}

void Messenger::notifyMain(Callback callback)
{
    Threads::callAsync(
        [impl = this->impl, callback = std::move(callback)]
        {
            if (!impl->stop)
                callback();
        });
}

// sessions is touched only on the main thread - the adoption closures run
// there, and so does the destructor - so it needs no lock.
struct MessageServer::Impl
{
    std::optional<ChannelServer> server;
    Vector<OwningPointer<Messenger>> sessions;

    std::atomic<bool> stop = false;
    std::thread acceptor;
};

MessageServer::MessageServer(std::string_view name)
    : impl(std::make_shared<Impl>())
{
    impl->server.emplace(name);
    impl->acceptor = std::thread([this] { acceptLoop(); });
}

MessageServer::~MessageServer()
{
    impl->stop = true;

    if (impl->acceptor.joinable())
        impl->acceptor.join();

    // The sessions die here, on the main thread, not whenever the last
    // queued delivery lets go of impl: each Messenger destructor is what
    // guarantees its callbacks never fire again.
    impl->sessions.clear();
}

void MessageServer::acceptLoop()
{
    while (!impl->stop)
    {
        auto accepted = std::optional<Channel> {};

        try
        {
            accepted = impl->server->accept(acceptSlice);
        }
        catch (const Error&)
        {
            return;
        }

        if (!accepted)
            continue;

        // The session reads from birth, on this thread's say-so: adoption
        // must not wait for a main-thread tick, because the main thread
        // may itself be blocked in a send() this reader has to drain (a
        // same-process peer greeting us with more than a socket buffer).
        // The wiring closure is queued BEFORE reading begins, so anything
        // the session hears lands behind it - onClient always runs first.
        auto* session = new Messenger(std::move(*accepted));

        Threads::callAsync(
            [impl = this->impl, this, session]
            {
                auto owned = OwningPointer<Messenger> {session};

                if (impl->stop)
                    return; // owned reaps the never-delivered session

                // Finished sessions have already delivered their
                // onDisconnected - a session finishes only after queueing
                // it, and this runs later - so sweeping cannot eat news.
                impl->sessions.eraseIf([](const OwningPointer<Messenger>& candidate)
                                       { return candidate->finished(); });

                auto& stored = impl->sessions.add(std::move(owned));
                onClient(*stored);
            });

        session->begin();
    }
}

} // namespace eacp::IPC
