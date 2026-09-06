#include "Backend.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Threads/ThreadUtils.h>

#include <atomic>
#include <thread>

namespace eacp::WebSocket
{
// The Sink every backend reports into, and the only place the connection's
// state moves. A report may arrive on any thread and at any time - a backend
// racing the destructor included - so each one is handed to the message
// thread holding a reference to this object, and dropped on arrival once the
// Connection that owns it has gone. Everything below the marshalling is
// therefore message-thread-only, and none of it is atomic but the one flag
// the closing handshake's timeout waits on from a thread of its own.
class WebSocketEndpoint
    : public Sink
    , public std::enable_shared_from_this<WebSocketEndpoint>
{
public:
    explicit WebSocketEndpoint(Callbacks callbacksToUse)
        : callbacks(std::move(callbacksToUse))
    {
    }

    void opened(const std::string& protocolToUse) override
    {
        deliver([protocolToUse](WebSocketEndpoint& self)
                { self.deliverOpened(protocolToUse); });
    }

    void received(Message message) override
    {
        deliver([message = std::move(message)](WebSocketEndpoint& self)
                { self.deliverReceived(message); });
    }

    void closed(int code, const std::string& reason) override
    {
        deliver([code, reason](WebSocketEndpoint& self)
                { self.deliverClosed({code, reason}); });
    }

    void failed(const std::string& error) override
    {
        deliver([error](WebSocketEndpoint& self) { self.deliverFailed(error); });
    }

    // The closing handshake ran out of time, which is a report like any other
    // and goes the same way: to the message thread, and nowhere at all once
    // the Connection has gone.
    void closeTimedOut()
    {
        deliver([](WebSocketEndpoint& self) { self.deliverCloseTimedOut(); });
    }

    State currentState() const { return state; }
    const std::string& selectedProtocol() const { return protocol; }

    bool hasClosed() const
    {
        return state == State::closing || state == State::closed;
    }

    // False the moment the close settles or the Connection goes, so the wait
    // can stop early instead of sleeping out a timeout nobody is owed.
    bool isAwaitingClose() const { return awaitingClose.load(); }

    void setDropBackend(std::function<void()> callback)
    {
        dropBackend = std::move(callback);
    }

    void noteCloseRequested()
    {
        closeRequested = true;
        state = State::closing;
        awaitingClose.store(true);
    }

    void stopDelivering()
    {
        alive = false;
        awaitingClose.store(false);
        dropBackend = [] {};
    }

private:
    template <typename Delivery>
    void deliver(const Delivery& delivery)
    {
        auto self = shared_from_this();

        Threads::callAsync(
            [self, delivery]
            {
                if (self->alive)
                    delivery(*self);
            });
    }

    void deliverOpened(const std::string& protocolToUse)
    {
        if (state != State::connecting)
            return;

        state = State::open;
        protocol = protocolToUse;
        callbacks.onOpen(protocol);
    }

    void deliverReceived(const Message& message)
    {
        if (state != State::open && state != State::closing)
            return;

        callbacks.onMessage(message);
    }

    void deliverClosed(const CloseStatus& status)
    {
        if (state == State::closed)
            return;

        settle();
        callbacks.onClose(status);
    }

    void deliverFailed(const std::string& error)
    {
        if (state == State::closed)
            return;

        if (!closeRequested)
            callbacks.onError(error);

        settle();
        callbacks.onClose({1006, {}});
    }

    // A peer that answers close() with neither a close frame nor an end of
    // stream: the transport goes without another word from it, and the
    // connection reads as the abnormal closure it is.
    void deliverCloseTimedOut()
    {
        if (state != State::closing)
            return;

        settle();
        dropBackend();
        callbacks.onClose({1006, {}});
    }

    void settle()
    {
        state = State::closed;
        awaitingClose.store(false);
    }

    Callbacks callbacks;
    std::function<void()> dropBackend = [] {};
    State state = State::connecting;
    std::string protocol;
    bool alive = true;
    bool closeRequested = false;
    std::atomic<bool> awaitingClose {false};
};

// How long the wait for the closing handshake naps between looks at whether
// it is still owed one.
Time::MS webSocketCloseWaitSlice(const Time::Deadline& deadline)
{
    constexpr auto slice = Time::MS {20};
    auto remaining = deadline.remaining();
    return remaining < slice ? remaining : slice;
}

struct Connection::Impl
{
    Impl(std::string urlToUse, Callbacks callbacks, const Options& options)
        : url(std::move(urlToUse))
        , endpoint(std::make_shared<WebSocketEndpoint>(std::move(callbacks)))
        , closeTimeout(options.closeTimeout)
    {
        endpoint->setDropBackend([this] { backend.reset(); });
        start(options);
    }

    ~Impl()
    {
        endpoint->stopDelivering();
        backend.reset();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void start(const Options& options)
    {
        try
        {
            backend = makeBackend(url, options, endpoint);
        }
        catch (const std::exception& e)
        {
            backend.reset();
            endpoint->failed(e.what());
            return;
        }

        if (backend == nullptr)
            endpoint->failed("WebSocket is unsupported on this platform");
    }

    void sendIfOpen(const Message& message)
    {
        if (backend == nullptr || endpoint->currentState() != State::open)
            return;

        backend->send(message);
    }

    void requestClose(int code, const std::string& reason)
    {
        if (endpoint->hasClosed())
            return;

        endpoint->noteCloseRequested();

        if (backend != nullptr)
            backend->close(code, reason);

        startCloseTimeout();
    }

    // Core has no one-shot timer, so the bound on the closing handshake is a
    // thread of its own, holding nothing but the endpoint - which is why a
    // Connection destroyed first leaves it nothing to dangle over. It looks
    // in as it waits rather than sleeping the whole timeout out, so a close
    // that settles leaves no thread behind it.
    void startCloseTimeout()
    {
        if (closeTimeout.count <= 0)
            return;

        auto wait = [waking = endpoint, timeout = closeTimeout]
        {
            auto deadline = Time::Deadline {timeout};

            while (!deadline.expired())
            {
                if (!waking->isAwaitingClose())
                    return;

                Time::sleep(webSocketCloseWaitSlice(deadline));
            }

            waking->closeTimedOut();
        };

        std::thread(wait).detach();
    }

    std::string url;
    std::shared_ptr<WebSocketEndpoint> endpoint;
    Time::MS closeTimeout;
    std::unique_ptr<Backend> backend;
};

Connection::Connection(const std::string& url,
                       Callbacks callbacks,
                       const Options& options)
{
    Threads::assertMainThread();
    impl.create(url, std::move(callbacks), options);
}

Connection::~Connection() = default;

bool Connection::isSupported()
{
    return backendIsSupported();
}

void Connection::send(std::string_view text)
{
    Threads::assertMainThread();
    impl->sendIfOpen({std::string(text), MessageType::text});
}

void Connection::sendBinary(std::string_view bytes)
{
    Threads::assertMainThread();
    impl->sendIfOpen({std::string(bytes), MessageType::binary});
}

void Connection::close(int code, std::string_view reason)
{
    Threads::assertMainThread();
    impl->requestClose(code, std::string(reason));
}

State Connection::state() const
{
    return impl->endpoint->currentState();
}

const std::string& Connection::url() const
{
    return impl->url;
}

const std::string& Connection::protocol() const
{
    return impl->endpoint->selectedProtocol();
}

} // namespace eacp::WebSocket
