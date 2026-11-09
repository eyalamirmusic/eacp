#include "Messenger.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace eacp::IPC
{
namespace
{
// A dial retries this often while nobody serves the name; the wait between
// attempts ends the moment the Messenger is destroyed.
constexpr auto dialRetry = Time::MS {25};

// The destructor interrupts the acceptor, so this only bounds a wake-up a
// platform loses.
constexpr auto acceptSlice = Time::MS {200};
constexpr auto interruptRetry = Time::MS {5};

// A frame is a 32-bit little-endian byte count, then that many bytes. The
// prefix is what frees payloads to carry anything - newlines, NULs, whole
// files.
constexpr auto headerSize = 4;

// Refuses lengths no honest peer would send, so a framing bug surfaces as
// an error instead of a gigabyte allocation.
constexpr auto maxMessageSize = 1 << 30;

// A peer that leaves this much unread has stopped reading; the conversation
// ends rather than the outbox growing without limit.
constexpr auto maxUnsentBytes = std::size_t {64} << 20;

// Teardown gives queued messages this long to reach a peer that is still
// reading, so a last word sent just before destruction is not lost. The wait
// runs on a thread of its own, never on the thread destroying the Messenger.
constexpr auto unsentFlushLimit = Time::MS {1000};

constexpr auto streamThreadCount = 2;

Array<char, headerSize> encodeHeader(int size)
{
    auto header = Array<char, headerSize> {};

    for (auto index = 0; index < headerSize; ++index)
        header[index] = (char) ((size >> (index * 8)) & 0xff);

    return header;
}

std::uint32_t decodeLength(const std::string& header)
{
    auto size = std::uint32_t {0};

    for (auto index = 0; index < headerSize; ++index)
        size |= (std::uint32_t) (unsigned char) header[(std::size_t) index]
                << (index * 8);

    return size;
}

// Fills result with exactly count bytes, sized once and written in place;
// false means the stream ended first, leaving what did arrive in result.
bool receiveExactly(Channel& channel, int count, std::string& result)
{
    result.resize((std::size_t) count);
    auto received = 0;

    while (received < count)
    {
        auto chunk = channel.receive(result.data() + received, count - received);

        if (chunk == 0)
        {
            result.resize((std::size_t) received);
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

    if (length > (std::uint32_t) maxMessageSize)
        throw Error("peer sent an implausible message length");

    auto message = std::string {};

    if (!receiveExactly(channel, (int) length, message))
        throw Error("peer closed the channel mid-message");

    return message;
}
} // namespace

// Shared with every delivery queued onto the main thread: a delivery that
// fires after the Messenger died still finds isOwnerGone (and backs off)
// instead of dangling. The channel is emplaced once and then only closed by
// whichever stream thread leaves last, so the reader and the writer use it
// without locking, as the Channel's one-sender-one-receiver contract allows.
struct Messenger::Impl : std::enable_shared_from_this<Impl>
{
    explicit Impl(Messenger& ownerToNotify)
        : owner(ownerToNotify)
    {
    }

    // Read only by deliveries on the main thread, and only while
    // isOwnerGone is false, which the destructor sets on that same thread.
    Messenger& owner;
    std::atomic<bool> isOwnerGone = false;

    std::string name;
    Time::MS timeout {};

    std::mutex mutex;
    std::optional<Channel> channel;
    std::atomic<bool> connected = false;
    std::atomic<bool> conversationEnded = false;

    std::atomic<bool> stop = false;
    std::atomic<bool> workerDone = false;
    std::atomic<bool> writerDone = false;
    std::atomic<int> streamThreadsRunning = streamThreadCount;
    std::thread worker;
    std::thread writer;

    std::mutex stopMutex;
    std::condition_variable stopSignal;

    std::deque<std::string> outbox;
    std::size_t unsentBytes = 0;

    void requestStop()
    {
        {
            auto guard = std::lock_guard {stopMutex};
            stop = true;
        }

        stopSignal.notify_all();
    }

    void endConversation()
    {
        {
            auto guard = std::lock_guard {stopMutex};
            connected = false;
            conversationEnded = true;
            outbox.clear();
            unsentBytes = 0;
        }

        stopSignal.notify_all();
    }

    void adoptDialedChannel(Channel opened)
    {
        {
            auto guard = std::lock_guard {mutex};
            channel.emplace(std::move(opened));
        }

        {
            auto guard = std::lock_guard {stopMutex};
            connected = true;
        }

        stopSignal.notify_all();
    }

    void interruptChannel()
    {
        auto guard = std::lock_guard {mutex};

        if (channel)
            channel->interrupt();
    }

    void closeChannelIfLastStreamThread()
    {
        if (streamThreadsRunning.fetch_sub(1) != 1 || stop)
            return;

        auto guard = std::lock_guard {mutex};
        channel.reset();
    }

    [[nodiscard]] bool hasMessageToWrite() const
    {
        return !stop && connected && !outbox.empty();
    }

    [[nodiscard]] bool isWriterNeeded() const
    {
        return stop || conversationEnded || workerDone || hasMessageToWrite();
    }

    void markSent(std::size_t byteCount)
    {
        {
            auto guard = std::lock_guard {stopMutex};

            if (!conversationEnded)
                unsentBytes -= byteCount;
        }

        stopSignal.notify_all();
    }

    void waitForUnsentMessagesToLeave()
    {
        auto guard = std::unique_lock {stopMutex};
        auto isFlushed = [this] { return conversationEnded || unsentBytes == 0; };
        stopSignal.wait_for(
            guard, std::chrono::milliseconds(unsentFlushLimit.count), isFlushed);
    }

    [[nodiscard]] bool areStreamThreadsDone() const
    {
        return workerDone && writerDone;
    }

    void waitBeforeRetrying(Time::MS interval)
    {
        auto guard = std::unique_lock {stopMutex};
        auto stopped = [this] { return stop.load(); };
        stopSignal.wait_for(
            guard, std::chrono::milliseconds(interval.count), stopped);
    }

    [[nodiscard]] bool hasUnsentMessages()
    {
        auto guard = std::lock_guard {stopMutex};
        return !conversationEnded && unsentBytes > 0;
    }

    void stopStreamThreads()
    {
        waitForUnsentMessagesToLeave();
        requestStop();

        // Interrupt until both stream threads acknowledge: on POSIX the first
        // pass wakes them for good; on Windows a cancel only reaches I/O
        // already in flight, so keep knocking (see Channel::interrupt).
        while (worker.joinable() && !areStreamThreadsDone())
        {
            interruptChannel();
            Time::sleep(interruptRetry);
        }

        if (worker.joinable())
            worker.join();

        if (writer.joinable())
            writer.join();
    }

    void work();
    bool dial();
    bool tryConnectingOnce();
    void readUntilGone();
    void finishReading();
    std::optional<std::string> takeNextUnsentMessage();
    void writeUntilGone();
    void interruptUntilReaderLeaves();
    void notifyMain(Callback callback);
};

Messenger::Messenger(std::string_view name, Time::MS timeout)
    : impl(std::make_shared<Impl>(*this))
{
    impl->name = std::string {name};
    impl->timeout = timeout;
    begin();
}

Messenger::Messenger(Channel connectedChannel)
    : impl(std::make_shared<Impl>(*this))
{
    impl->channel.emplace(std::move(connectedChannel));
    impl->connected = true;
}

// A peer that stopped reading would hold the flush wait for its full second;
// with messages still queued, that wait moves to a thread of its own, which
// keeps impl alive until both stream threads have been joined.
Messenger::~Messenger()
{
    impl->isOwnerGone = true;

    if (impl->hasUnsentMessages())
    {
        std::thread([sharedImpl = impl] { sharedImpl->stopStreamThreads(); })
            .detach();
        return;
    }

    impl->stopStreamThreads();
}

void Messenger::begin()
{
    impl->writer = std::thread([&self = *impl] { self.writeUntilGone(); });
    impl->worker = std::thread([&self = *impl] { self.work(); });
}

void Messenger::abandonUnsentMessages()
{
    impl->endConversation();
}

bool Messenger::finished() const
{
    return impl->areStreamThreadsDone();
}

bool Messenger::hasUnsentMessages() const
{
    return impl->hasUnsentMessages();
}

bool Messenger::isConnected() const
{
    return impl->connected;
}

void Messenger::send(std::string message)
{
    if (message.size() > (std::size_t) maxMessageSize)
        throw Error("message is too large to frame");

    auto isOverflowing = false;

    {
        auto guard = std::lock_guard {impl->stopMutex};

        if (!impl->connected)
            return;

        isOverflowing = !impl->outbox.empty()
                        && impl->unsentBytes + message.size() > maxUnsentBytes;

        if (!isOverflowing)
        {
            const auto messageSize = message.size();
            impl->outbox.push_back(std::move(message));
            impl->unsentBytes += messageSize;
        }
    }

    if (isOverflowing)
    {
        impl->endConversation();
        impl->interruptChannel();
        return;
    }

    impl->stopSignal.notify_all();
}

std::optional<std::string> Messenger::Impl::takeNextUnsentMessage()
{
    auto guard = std::unique_lock {stopMutex};
    stopSignal.wait(guard, [this] { return isWriterNeeded(); });

    if (!hasMessageToWrite())
        return std::nullopt;

    auto message = std::move(outbox.front());
    outbox.pop_front();
    return message;
}

void Messenger::Impl::writeUntilGone()
{
    try
    {
        while (auto message = takeNextUnsentMessage())
        {
            auto header = encodeHeader((int) message->size());
            channel->send({header.data(), (std::size_t) headerSize});
            channel->send(*message);
            markSent(message->size());
        }
    }
    catch (const Error&)
    {
        endConversation();
    }

    interruptUntilReaderLeaves();
    writerDone = true;
    closeChannelIfLastStreamThread();
}

void Messenger::Impl::interruptUntilReaderLeaves()
{
    while (!workerDone && !stop)
    {
        interruptChannel();
        Time::sleep(interruptRetry);
    }
}

void Messenger::Impl::work()
{
    auto dialed = !name.empty();

    if (dialed && !dial())
    {
        if (!stop)
            notifyMain([this] { owner.onDisconnected(); });

        finishReading();
        return;
    }

    if (dialed)
        notifyMain([this] { owner.onConnected(); });

    readUntilGone();
    finishReading();
}

void Messenger::Impl::finishReading()
{
    {
        auto guard = std::lock_guard {stopMutex};
        connected = false;
        conversationEnded = true;
        workerDone = true;
    }

    stopSignal.notify_all();
    closeChannelIfLastStreamThread();
}

bool Messenger::Impl::tryConnectingOnce()
{
    try
    {
        adoptDialedChannel(Channel::connect(name, Time::MS {0}));
        return true;
    }
    catch (const Error&)
    {
        return false;
    }
}

bool Messenger::Impl::dial()
{
    auto deadline = Time::Deadline {timeout};

    while (!stop)
    {
        if (tryConnectingOnce())
            return true;

        if (deadline.expired())
            return false;

        auto remaining = deadline.remaining();
        waitBeforeRetrying(remaining < dialRetry ? remaining : dialRetry);
    }

    return false;
}

void Messenger::Impl::readUntilGone()
{
    try
    {
        for (;;)
        {
            auto message = receiveFrame(*channel);

            // An interrupted read surfaces as end of stream; the flag is
            // what tells a teardown apart from the peer leaving.
            if (stop)
                return;

            if (!message)
                break;

            notifyMain([this, delivered = std::move(*message)]() mutable
                       { owner.onMessage(std::move(delivered)); });
        }
    }
    catch (const Error&)
    {
        // A broken stream ends the conversation the same way a clean
        // close does.
    }

    if (stop)
        return;

    endConversation();
    notifyMain([this] { owner.onDisconnected(); });
}

void Messenger::Impl::notifyMain(Callback callback)
{
    if (isOwnerGone)
        return;

    Threads::callAsync(
        [sharedImpl = shared_from_this(), onMain = std::move(callback)]
        {
            if (!sharedImpl->isOwnerGone)
                onMain();
        });
}

// sessions is touched only on the main thread - the adoption closures run
// there, and so does the destructor - so it needs no lock.
struct MessageServer::Impl
{
    std::optional<ChannelServer> server;
    Vector<OwningPointer<Messenger>> sessions;
    Vector<OwningPointer<Messenger>> ending;

    std::atomic<bool> stop = false;
    std::thread acceptor;

    // A session queues its onDisconnected before it reports finished, and
    // ~Messenger drops an undelivered one - so the reap waits behind it.
    void reapFinishedSessionsLater(const std::shared_ptr<Impl>& self)
    {
        sessions.eraseIf(
            [this](OwningPointer<Messenger>& candidate)
            {
                if (!candidate->finished())
                    return false;

                ending.add(std::move(candidate));
                return true;
            });

        if (!ending.empty())
            Threads::callAsync([self] { self->ending.clear(); });
    }
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
    impl->server->interrupt();

    if (impl->acceptor.joinable())
        impl->acceptor.join();

    // The sessions die here, on the main thread, not whenever the last
    // queued delivery lets go of impl: each Messenger destructor is what
    // guarantees its callbacks never fire again. A closing server owes its
    // clients nothing more, so no session waits to flush.
    for (auto& session: impl->sessions)
        session->abandonUnsentMessages();

    impl->sessions.clear();
    impl->ending.clear();
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

                impl->reapFinishedSessionsLater(impl);

                auto& stored = impl->sessions.add(std::move(owned));
                onClient(*stored);
            });

        session->begin();
    }
}

} // namespace eacp::IPC
