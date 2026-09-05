#import <Foundation/Foundation.h>
#include "Backend.h"
#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/Core/ObjC/RuntimeClass.h>
#include <eacp/Core/ObjC/Strings.h>

#include <mutex>

namespace eacp::WebSocket
{
// What the delegate, the completion blocks and the backend share. The blocks
// and the delegate both outlive the backend - a cancelled task reports on the
// session's queue long after the Connection has gone - so everything they
// touch lives here behind a lock, held by shared_ptr rather than by the
// backend that made it.
//
// It is also the gate that keeps the Sink's contract: one terminal report and
// nothing after it, however many ways the task has of ending.
struct WebSocketContext
{
    explicit WebSocketContext(std::shared_ptr<Sink> sinkToUse)
        : sink(std::move(sinkToUse))
    {
    }

    void opened(const std::string& protocol)
    {
        auto lock = std::scoped_lock(mutex);

        if (!finished)
            sink->opened(protocol);
    }

    void received(Message message)
    {
        auto lock = std::scoped_lock(mutex);

        if (!finished)
            sink->received(std::move(message));
    }

    void notePeerClose(int code, std::string reason)
    {
        auto lock = std::scoped_lock(mutex);

        sawPeerClose = true;
        peerCode = code;
        peerReason = std::move(reason);
    }

    // Why the transport stopped, kept rather than reported: a send or a
    // receive that fails is the task ending, and the task says how.
    void noteTransportError(std::string error)
    {
        auto lock = std::scoped_lock(mutex);

        if (transportError.empty())
            transportError = std::move(error);
    }

    // The one place a task's ending is turned into a report: a close code
    // beats the error that stopped us, which beats a stream that simply
    // ended. The order is not a preference - a perfectly clean close leaves
    // "Socket is not connected" behind on the pending receive, so an error
    // read first would turn every closing handshake into a failure.
    //
    // The limit of the platform is that didCloseWithCode: also fires for our
    // own cancelWithCloseCode:, carrying back the code we passed, so after a
    // close of ours the code is what we asked for whether the peer answered
    // it or not. A closing handshake the peer abandons therefore reads here
    // as the close that was asked for, not as §7.1.5's 1006.
    void finish(int taskCloseCode,
                const std::string& taskCloseReason,
                const std::string& error)
    {
        auto lock = std::scoped_lock(mutex);

        if (finished)
            return;

        finished = true;

        if (sawPeerClose)
        {
            sink->closed(peerCode, peerReason);
            return;
        }

        if (taskCloseCode != 0)
        {
            sink->closed(taskCloseCode, taskCloseReason);
            return;
        }

        auto failure = error.empty() ? transportError : error;

        if (!failure.empty())
        {
            sink->failed(failure);
            return;
        }

        sink->closed(1005, {});
    }

    // The backend is going away: whatever the cancelled task reports next is
    // our own teardown answering itself, and no business of the Sink's.
    void detach()
    {
        auto lock = std::scoped_lock(mutex);
        finished = true;
    }

    std::mutex mutex;
    std::shared_ptr<Sink> sink;
    bool finished = false;
    bool sawPeerClose = false;
    int peerCode = 1005;
    std::string peerReason;
    std::string transportError;
};

namespace
{
using WebSocketContextBox = std::shared_ptr<WebSocketContext>;

WebSocketContextBox* webSocketBoxOf(id self)
{
    return (WebSocketContextBox*) ObjC::getIvar<void*>(self, "ctx");
}

WebSocketContext* webSocketContextOf(id self)
{
    auto* box = webSocketBoxOf(self);
    return box != nullptr ? box->get() : nullptr;
}

std::string webSocketStringOf(NSURLSessionWebSocketMessage* message)
{
    if (message.type == NSURLSessionWebSocketMessageTypeString)
        return Strings::toStdString(message.string);

    return Strings::toStdString(message.data);
}

Message webSocketMessageFrom(NSURLSessionWebSocketMessage* message)
{
    auto isText = message.type == NSURLSessionWebSocketMessageTypeString;
    return {webSocketStringOf(message),
            isText ? MessageType::text : MessageType::binary};
}

// Armed once the task is resumed - it queues until the socket opens - and
// again after every message, so a delivery is always waiting. An error ends
// the loop: the task's own completion is what reports it.
void webSocketArmReceive(NSURLSessionWebSocketTask* task,
                         WebSocketContextBox context)
{
    [task receiveMessageWithCompletionHandler:
              ^(NSURLSessionWebSocketMessage* message, NSError* error) {
                if (error != nil)
                {
                    context->noteTransportError(Strings::toStdString(error));
                    return;
                }

                if (message == nil)
                    return;

                context->received(webSocketMessageFrom(message));
                webSocketArmReceive(task, context);
              }];
}

void webSocketDelegateDidOpen(id self,
                              SEL,
                              NSURLSession*,
                              NSURLSessionWebSocketTask*,
                              NSString* protocol)
{
    if (auto* context = webSocketContextOf(self))
        context->opened(Strings::toStdString(protocol));
}

void webSocketDelegateDidClose(id self,
                               SEL,
                               NSURLSession*,
                               NSURLSessionWebSocketTask*,
                               NSURLSessionWebSocketCloseCode code,
                               NSData* reason)
{
    if (auto* context = webSocketContextOf(self))
        context->notePeerClose((int) code, Strings::toStdString(reason));
}

void webSocketDelegateDidComplete(
    id self, SEL, NSURLSession*, NSURLSessionTask* task, NSError* error)
{
    auto* context = webSocketContextOf(self);

    if (context == nullptr)
        return;

    auto code = 0;
    auto reason = std::string();

    if ([task isKindOfClass:[NSURLSessionWebSocketTask class]])
    {
        auto* socketTask = (NSURLSessionWebSocketTask*) task;
        code = (int) socketTask.closeCode;
        reason = Strings::toStdString(socketTask.closeReason);
    }

    context->finish(code, reason, Strings::toStdString(error));
}

// The last message a session sends its delegate, and so where the delegate's
// half of the shared context is let go: the session holds the delegate until
// this returns, and nothing arrives after it.
void webSocketDelegateDidInvalidate(id self, SEL, NSURLSession*, NSError*)
{
    auto*& slot = ObjC::getIvar<void*>(self, "ctx");
    delete (WebSocketContextBox*) slot;
    slot = nullptr;
}

Class webSocketDelegateClass()
{
    static auto* instance = []
    {
        auto* builder = new ObjC::RuntimeClass<NSObject>("EacpWebSocketDelegate");

        builder->addIvar<void*>("ctx");
        builder->addProtocol(@protocol(NSURLSessionWebSocketDelegate));

        builder->addMethod(@selector(URLSession:webSocketTask:didOpenWithProtocol:),
                           webSocketDelegateDidOpen);
        builder->addMethod(@selector(URLSession:
                                     webSocketTask:didCloseWithCode:reason:),
                           webSocketDelegateDidClose);
        builder->addMethod(@selector(URLSession:task:didCompleteWithError:),
                           webSocketDelegateDidComplete);
        builder->addMethod(@selector(URLSession:didBecomeInvalidWithError:),
                           webSocketDelegateDidInvalidate);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}

double webSocketSeconds(Time::MS timeout)
{
    return (double) timeout.count / 1000.0;
}

std::string webSocketJoinProtocols(const Vector<std::string>& protocols)
{
    auto joined = std::string();

    for (const auto& protocol: protocols)
    {
        if (!joined.empty())
            joined += ", ";

        joined += protocol;
    }

    return joined;
}

NSMutableURLRequest* webSocketRequestFor(const std::string& url,
                                         const Options& options)
{
    if (url.empty())
        throw std::invalid_argument("URL cannot be empty");

    auto* urlString = Strings::toNSString(url);

    if (urlString == nil)
        throw std::runtime_error("URL contains invalid UTF-8 characters");

    auto* parsed = [NSURL URLWithString:urlString];

    if (parsed == nil)
        throw std::runtime_error("Malformed URL format");

    auto* request = [NSMutableURLRequest requestWithURL:parsed];

    for (const auto& header: options.headers)
    {
        auto* name = Strings::toNSString(header.first);
        auto* value = Strings::toNSString(header.second);

        if (name != nil && value != nil)
            [request setValue:value forHTTPHeaderField:name];
    }

    auto offered = webSocketJoinProtocols(options.protocols);

    if (!offered.empty())
        [request setValue:Strings::toNSString(offered)
            forHTTPHeaderField:@"Sec-WebSocket-Protocol"];

    if (options.connectTimeout.count > 0)
        request.timeoutInterval = webSocketSeconds(options.connectTimeout);

    return request;
}

// Nil for a text payload that is not UTF-8, which RFC 6455 §5.6 has no frame
// for: initWithBytes: answers nil there and NSURLSessionWebSocketMessage
// raises on a nil string. Built from the bytes rather than from a C string so
// a legitimate U+0000 survives.
NSURLSessionWebSocketMessage* webSocketMessageFor(const Message& message)
{
    if (message.type == MessageType::binary)
        return [[[NSURLSessionWebSocketMessage alloc]
            initWithData:Strings::toNSData(message.data)] autorelease];

    auto* text = [[[NSString alloc] initWithBytes:message.data.data()
                                           length:message.data.size()
                                         encoding:NSUTF8StringEncoding]
        autorelease];

    if (text == nil)
        return nil;

    return [[[NSURLSessionWebSocketMessage alloc] initWithString:text]
        autorelease];
}

// RFC 6455 §5.5: a close reason is what is left of a control frame's 125
// bytes once the code has taken two, and Foundation raises rather than
// truncating.
std::string webSocketTrimReason(const std::string& reason)
{
    constexpr auto maxReasonBytes = std::size_t {123};
    return reason.size() <= maxReasonBytes ? reason
                                           : reason.substr(0, maxReasonBytes);
}

class WebSocketAppleBackend final : public Backend
{
public:
    WebSocketAppleBackend(const std::string& url,
                          const Options& options,
                          std::shared_ptr<Sink> sink)
        : context(std::make_shared<WebSocketContext>(std::move(sink)))
    {
        auto pool = ObjC::AutoReleasePool();

        auto* request = webSocketRequestFor(url, options);

        delegate.set([[webSocketDelegateClass() alloc] init]);
        ObjC::getIvar<void*>(delegate.get(), "ctx") =
            new WebSocketContextBox(context);

        auto* configuration =
            [NSURLSessionConfiguration defaultSessionConfiguration];

        if (options.connectTimeout.count > 0)
            configuration.timeoutIntervalForRequest =
                webSocketSeconds(options.connectTimeout);

        session = ObjC::attachPtr([NSURLSession
            sessionWithConfiguration:configuration
                            delegate:(id<NSURLSessionDelegate>) delegate.get()
                       delegateQueue:nil]);

        task = ObjC::attachPtr([session.get() webSocketTaskWithRequest:request]);
        task.get().maximumMessageSize = (NSInteger) options.maxMessageSize;

        [task.get() resume];
        webSocketArmReceive(task.get(), context);
    }

    ~WebSocketAppleBackend() override
    {
        auto pool = ObjC::AutoReleasePool();

        context->detach();
        [session.get() invalidateAndCancel];
    }

    void send(const Message& message) override
    {
        auto pool = ObjC::AutoReleasePool();
        auto* payload = webSocketMessageFor(message);

        if (payload == nil)
        {
            context->noteTransportError("Text message is not valid UTF-8");
            return;
        }

        auto forReporting = context;

        [task.get() sendMessage:payload
              completionHandler:^(NSError* error) {
                if (error != nil)
                    forReporting->noteTransportError(Strings::toStdString(error));
              }];
    }

    void close(int code, const std::string& reason) override
    {
        auto pool = ObjC::AutoReleasePool();
        auto trimmed = webSocketTrimReason(reason);
        NSData* payload = nil;

        if (!trimmed.empty())
            payload = Strings::toNSData(trimmed);

        [task.get() cancelWithCloseCode:(NSURLSessionWebSocketCloseCode) code
                                 reason:payload];
    }

private:
    std::shared_ptr<WebSocketContext> context;
    ObjC::Ptr<NSObject> delegate;
    ObjC::Ptr<NSURLSession> session;
    ObjC::Ptr<NSURLSessionWebSocketTask> task;
};
} // namespace

std::unique_ptr<Backend> makeBackend(const std::string& url,
                                     const Options& options,
                                     std::shared_ptr<Sink> sink)
{
    return std::make_unique<WebSocketAppleBackend>(url, options, std::move(sink));
}

bool backendIsSupported()
{
    return true;
}

} // namespace eacp::WebSocket
