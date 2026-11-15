#include "Backend.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Threads/ThreadUtils.h>

namespace eacp::WebSocket
{
// The Sink every backend reports into, and the only place the connection's
// state moves. A report may arrive on any thread and at any time - a backend
// racing the destructor included - so each one is handed to the message
// thread holding a reference to this object, and dropped on arrival once the
// Connection that owns it has gone. Everything below the marshalling is
// therefore message-thread-only.
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
        deliver([incoming = std::move(message)](WebSocketEndpoint& self)
                { self.deliverReceived(incoming); });
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

    // The closing handshake ran out of time. Called on the message thread by
    // the callAfter that bounds it, so it needs no marshalling, only the same
    // silence once the Connection has gone.
    void closeTimedOut()
    {
        if (alive)
            deliverCloseTimedOut();
    }

    State currentState() const { return state; }
    const std::string& selectedProtocol() const { return protocol; }

    bool hasClosed() const
    {
        return state == State::closing || state == State::closed;
    }

    void setDropBackend(std::function<void()> callback)
    {
        dropBackend = std::move(callback);
    }

    void noteCloseRequested()
    {
        closeRequested = true;
        state = State::closing;
    }

    void stopDelivering()
    {
        alive = false;
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

    void settle() { state = State::closed; }

    Callbacks callbacks;
    std::function<void()> dropBackend = [] {};
    State state = State::connecting;
    std::string protocol;
    bool alive = true;
    bool closeRequested = false;
};

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

    // The bound on the closing handshake holds the endpoint weakly, so a
    // Connection destroyed first leaves it nothing to keep alive, and a close
    // that settled first makes it a no-op.
    void startCloseTimeout()
    {
        if (closeTimeout.count <= 0)
            return;

        Threads::callAfter(closeTimeout,
                           [weak = std::weak_ptr(endpoint)]
                           {
                               if (auto waking = weak.lock())
                                   waking->closeTimedOut();
                           });
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
