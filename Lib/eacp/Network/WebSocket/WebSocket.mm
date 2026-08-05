#import <Foundation/Foundation.h>

#include "WebSocket.h"

#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/RuntimeClass.h>
#include <eacp/Core/ObjC/Strings.h>
#include <eacp/Core/Threads/TaskSemaphore.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace eacp::WebSocket
{
namespace
{
// Everything the delegate writes and the Connection reads. Heap-allocated
// and owned by the Impl so the address handed to the delegate stays valid
// for as long as the session can still call back.
struct SessionState
{
    Threads::TaskSemaphore settled;

    std::mutex mutex;
    std::string subprotocol;
    std::string error;
    std::string closeReason;

    std::atomic<int> closeCode {0};
    std::atomic<bool> opened {false};
    std::atomic<bool> finished {false};

    void finish(std::string message)
    {
        {
            auto lock = std::lock_guard(mutex);

            if (error.empty())
                error = std::move(message);
        }

        finished.store(true);
        settled.signal();
    }

    std::string takeError()
    {
        auto lock = std::lock_guard(mutex);
        return error;
    }
};

SessionState* delegateState(id self)
{
    return (SessionState*) ObjC::getIvar<void*>(self, "state");
}

void delegateDidOpen(id self,
                     SEL,
                     NSURLSession*,
                     NSURLSessionWebSocketTask*,
                     NSString* protocol)
{
    auto* state = delegateState(self);

    {
        auto lock = std::lock_guard(state->mutex);
        state->subprotocol = protocol ? Strings::toStdString(protocol) : "";
    }

    state->opened.store(true);
    state->settled.signal();
}

void delegateDidClose(id self,
                      SEL,
                      NSURLSession*,
                      NSURLSessionWebSocketTask*,
                      NSURLSessionWebSocketCloseCode code,
                      NSData* reason)
{
    auto* state = delegateState(self);

    {
        auto lock = std::lock_guard(state->mutex);
        state->closeReason = reason ? Strings::toStdString(reason) : "";
    }

    state->closeCode.store((int) code);
    state->finished.store(true);
    state->settled.signal();
}

// Also the handshake-failure path: a server that rejects the upgrade never
// produces didOpen, so without this the connect() wait would only end at
// its timeout.
void delegateDidComplete(id self,
                         SEL,
                         NSURLSession*,
                         NSURLSessionTask*,
                         NSError* error)
{
    auto* state = delegateState(self);

    if (error)
        state->finish(Strings::toStdString(error));
    else
    {
        state->finished.store(true);
        state->settled.signal();
    }
}

Class getDelegateClass()
{
    static auto instance = []
    {
        auto builder = new ObjC::RuntimeClass<NSObject>("EacpWebSocketDelegate");

        builder->addIvar<void*>("state");
        builder->addProtocol(@protocol(NSURLSessionWebSocketDelegate));

        builder->addMethod(
            @selector(URLSession:webSocketTask:didOpenWithProtocol:),
            delegateDidOpen);
        builder->addMethod(
            @selector(URLSession:webSocketTask:didCloseWithCode:reason:),
            delegateDidClose);
        builder->addMethod(@selector(URLSession:task:didCompleteWithError:),
                           delegateDidComplete);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

// One in-flight receive. Shared with the completion block so a callback that
// arrives after a TimeoutError writes somewhere still alive rather than into
// a dead stack frame.
struct ReceiveSlot
{
    Threads::TaskSemaphore done;
    ObjC::Ptr<NSURLSessionWebSocketMessage> message;
    ObjC::Ptr<NSError> error;
    std::atomic<bool> delivered {false};
};

struct SendSlot
{
    Threads::TaskSemaphore done;
    ObjC::Ptr<NSError> error;
};

NSURLSessionWebSocketMessage* toNativeMessage(const Message& message)
{
    if (message.isBinary())
    {
        auto data = Strings::toNSData(message.data);
        return [[NSURLSessionWebSocketMessage alloc] initWithData:data];
    }

    auto text = Strings::toNSString(message.data);

    if (!text)
        throw Error("WebSocket text frame is not valid UTF-8");

    return [[NSURLSessionWebSocketMessage alloc] initWithString:text];
}

Message fromNativeMessage(NSURLSessionWebSocketMessage* native)
{
    if (native.type == NSURLSessionWebSocketMessageTypeData)
        return Message::binary(Strings::toStdString(native.data));

    return Message::text(Strings::toStdString(native.string));
}
} // namespace

struct Connection::Impl
{
    ~Impl()
    {
        if (session)
            [session.get() invalidateAndCancel];
    }

    ObjC::Ptr<NSObject> delegate;
    ObjC::Ptr<NSURLSession> session;
    ObjC::Ptr<NSURLSessionWebSocketTask> task;

    // Heap-allocated: the delegate holds a raw pointer to it, and the Impl
    // itself moves when the Connection does.
    std::unique_ptr<SessionState> state = std::make_unique<SessionState>();

    // Carried across a timed-out receive so the next call waits on the same
    // outstanding request instead of queueing a second one, which would drop
    // the message the first is still waiting for.
    std::shared_ptr<ReceiveSlot> pendingReceive;

    Options options;
    bool closeSent = false;
};

Connection::Connection() = default;
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;

Connection::~Connection()
{
    if (impl)
        close();
}

namespace
{
NSMutableURLRequest* buildRequest(const std::string& url, const Options& options)
{
    auto urlString = Strings::toNSString(url);

    if (!urlString)
        throw Error("WebSocket URL contains invalid UTF-8: " + url);

    auto nsUrl = [NSURL URLWithString:urlString];

    if (!nsUrl)
        throw Error("Malformed WebSocket URL: " + url);

    auto request = [NSMutableURLRequest requestWithURL:nsUrl];

    if (options.timeouts.connect.count > 0)
        request.timeoutInterval = (double) options.timeouts.connect.count / 1000.0;

    for (const auto& [key, value]: options.headers)
    {
        auto k = Strings::toNSString(key);
        auto v = Strings::toNSString(value);

        if (k && v)
            [request setValue:v forHTTPHeaderField:k];
    }

    if (!options.subprotocols.empty())
    {
        auto joined = std::string();

        for (auto i = 0; i < options.subprotocols.size(); ++i)
        {
            if (i > 0)
                joined += ", ";

            joined += options.subprotocols[i];
        }

        [request setValue:Strings::toNSString(joined)
            forHTTPHeaderField:@"Sec-WebSocket-Protocol"];
    }

    return request;
}
} // namespace

Connection Connection::connect(const std::string& url, Options options)
{
    auto pool = ObjC::AutoReleasePool();

    // Parsed even though NSURL does the real work: it rejects a wrong scheme
    // with a message naming the problem, where NSURL would open an http://
    // URL as a doomed plain request.
    parseUrl(url);

    auto connection = Connection();
    connection.impl.create();

    auto& impl = *connection.impl;
    impl.options = options;

    auto request = buildRequest(url, options);

    impl.delegate.reset([[getDelegateClass() alloc] init]);
    ObjC::getIvar<void*>(impl.delegate.get(), "state") = impl.state.get();

    auto config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    impl.session.reset([NSURLSession
        sessionWithConfiguration:config
                        delegate:(id<NSURLSessionDelegate>) impl.delegate.get()
                   delegateQueue:nil]);

    impl.task.reset([impl.session.get() webSocketTaskWithRequest:request]);
    impl.task.get().maximumMessageSize = (NSInteger) options.maxMessageBytes;

    [impl.task.get() resume];

    if (!impl.state->settled.waitFor(options.timeouts.connect))
        throw TimeoutError("Timed out opening WebSocket to " + url);

    auto error = impl.state->takeError();

    if (!error.empty())
        throw Error("WebSocket handshake failed for " + url + ": " + error);

    if (!impl.state->opened.load())
        throw Error("WebSocket closed before the handshake completed: " + url);

    return connection;
}

bool Connection::isOpen() const
{
    return impl && impl->state->opened.load() && !impl->state->finished.load();
}

const std::string& Connection::subprotocol() const
{
    auto lock = std::lock_guard(impl->state->mutex);
    return impl->state->subprotocol;
}

void Connection::send(const Message& message)
{
    if (!impl)
        throw Error("send() on a moved-from WebSocket connection");

    if (impl->state->finished.load())
        throw Error("send() on a closed WebSocket connection");

    auto pool = ObjC::AutoReleasePool();
    auto slot = std::make_shared<SendSlot>();
    auto native = toNativeMessage(message);

    [impl->task.get() sendMessage:native
               completionHandler:^(NSError* error) {
                 slot->error.reset(error);
                 slot->done.signal();
               }];

    if (!slot->done.waitFor(impl->options.timeouts.io))
        throw TimeoutError("Timed out sending a WebSocket message");

    if (slot->error)
        throw Error("WebSocket send failed: "
                    + Strings::toStdString(slot->error.get()));
}

void Connection::sendText(std::string payload)
{
    send(Message::text(std::move(payload)));
}

void Connection::sendBinary(std::string payload)
{
    send(Message::binary(std::move(payload)));
}

std::optional<Message> Connection::receive()
{
    if (!impl)
        throw Error("receive() on a moved-from WebSocket connection");

    auto pool = ObjC::AutoReleasePool();

    auto slot = impl->pendingReceive;

    if (!slot)
    {
        if (impl->state->finished.load())
            return {};

        slot = std::make_shared<ReceiveSlot>();
        impl->pendingReceive = slot;

        [impl->task.get() receiveMessageWithCompletionHandler:^(
                              NSURLSessionWebSocketMessage* message,
                              NSError* error) {
          slot->message.reset(message);
          slot->error.reset(error);
          slot->delivered.store(true);
          slot->done.signal();
        }];
    }

    if (!slot->done.waitFor(impl->options.timeouts.io))
        throw TimeoutError("Timed out waiting for a WebSocket message");

    impl->pendingReceive.reset();

    if (slot->error)
    {
        // A close that arrived while the read was outstanding is the normal
        // end of a stream, not a failure worth throwing over.
        if (impl->state->finished.load() && impl->state->takeError().empty())
            return {};

        throw Error("WebSocket receive failed: "
                    + Strings::toStdString(slot->error.get()));
    }

    if (!slot->message)
        return {};

    return fromNativeMessage(slot->message.get());
}

void Connection::ping()
{
    if (!impl)
        throw Error("ping() on a moved-from WebSocket connection");

    auto pool = ObjC::AutoReleasePool();
    auto slot = std::make_shared<SendSlot>();

    [impl->task.get() sendPingWithPongReceiveHandler:^(NSError* error) {
      slot->error.reset(error);
      slot->done.signal();
    }];

    if (!slot->done.waitFor(impl->options.timeouts.io))
        throw TimeoutError("Timed out waiting for a WebSocket pong");

    if (slot->error)
        throw Error("WebSocket ping failed: "
                    + Strings::toStdString(slot->error.get()));
}

void Connection::close(int code, const std::string& reason)
{
    if (!impl || impl->closeSent)
        return;

    impl->closeSent = true;

    auto pool = ObjC::AutoReleasePool();
    auto reasonData = reason.empty() ? nil : Strings::toNSData(reason);

    [impl->task.get() cancelWithCloseCode:(NSURLSessionWebSocketCloseCode) code
                                   reason:reasonData];

    // Bounded rather than indefinite: a peer that never answers the close
    // handshake must not wedge a destructor.
    impl->state->settled.waitFor(Time::MS {2000});
}

int Connection::closeCode() const
{
    return impl->state->closeCode.load();
}

const std::string& Connection::closeReason() const
{
    auto lock = std::lock_guard(impl->state->mutex);
    return impl->state->closeReason;
}

} // namespace eacp::WebSocket
