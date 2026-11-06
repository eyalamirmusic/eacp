#include "HttpServer.h"
#include "../Common-Posix.h"
#include "HttpServerDispatcher.h"

#include "../HTTP/HttpProtocol.h"

#include <CoreFoundation/CoreFoundation.h>

#include <fcntl.h>

#include <mutex>
#include <set>
#include <thread>

namespace eacp::HTTP
{

namespace
{

constexpr auto streamingBodyReadChunk = 64 * 1024;

struct Connection
{
    CFSocketRef socket = nullptr;
    CFRunLoopSourceRef source = nullptr;
    RequestParser parser;
    std::string remoteAddr;
    int remotePort = -1;
    bool hasLookedForStreamingRoute = false;
};

// A streaming request's thread, kept until it has finished so stop() can wait
// for it and the next streaming request can join it.
struct StreamingThread
{
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> hasFinished;
};

void ignoreSigPipe()
{
    static auto once = []
    {
        ::signal(SIGPIPE, SIG_IGN);
        return 0;
    }();
    (void) once;
}

void sendAll(int fd, std::string_view payload)
{
    auto total = (int) payload.size();
    auto sent = 0;

    while (sent < total)
    {
        auto n = ::send(fd, payload.data() + sent, (std::size_t) (total - sent), 0);

        if (n <= 0)
            break;

        sent += (int) n;
    }
}

void writeResponseToFd(int fd, const Response& res)
{
    sendResponse(res, [fd](std::string_view bytes) { sendAll(fd, bytes); });
}

std::string receiveFromFd(int fd)
{
    auto received = std::string(streamingBodyReadChunk, '\0');
    auto n = ::recv(fd, received.data(), received.size(), 0);

    if (n <= 0)
        return {};

    received.resize((std::size_t) n);
    return received;
}

void makeBlocking(int fd)
{
    auto flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

void readPeerEndpoint(int fd, std::string& addr, int& port)
{
    auto peer = sockaddr_in {};
    auto peerLen = (socklen_t) sizeof(peer);

    if (getpeername(fd, (sockaddr*) &peer, &peerLen) != 0)
        return;

    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    addr = ip;
    port = (int) ntohs(peer.sin_port);
}

in_addr_t bindAddress(BindInterface bindTo)
{
    return htonl(bindTo == BindInterface::any ? INADDR_ANY : INADDR_LOOPBACK);
}

} // namespace

struct Server::Impl
{
    explicit Impl(ServerOptions opts)
        : options(opts), dispatcher(makeDispatcher(opts))
    {
    }

    ~Impl();

    bool start(int port, RequestHandler h, StreamingRouteFinder finder);
    void stop();

    void onAccept(CFSocketNativeHandle clientFd);
    void onClientReadable(CFSocketRef cf);
    void closeConnection(CFSocketRef cf);

    void detachKeepingFdOpen(Connection& conn);
    void detachClosingFd(Connection& conn);
    void dispatchRequest(int fd, Request request);
    bool startStreamingIfRouted(Connection& conn, int fd);
    void startStreamingThread(int fd,
                              Request head,
                              std::string bytesAfterHeaders,
                              StreamingRequestHandler streamingHandler);
    void serveStreamingFd(int fd,
                          const Request& head,
                          std::string bytesAfterHeaders,
                          const StreamingRequestHandler& streamingHandler);
    void joinFinishedStreamingThreads();
    void stopStreaming();

    static void acceptCallback(CFSocketRef, CFSocketCallBackType, CFDataRef,
                               const void* data, void* info)
    {
        auto self = static_cast<Impl*>(info);
        self->onAccept(*static_cast<const CFSocketNativeHandle*>(data));
    }

    static void clientCallback(CFSocketRef cf, CFSocketCallBackType,
                               CFDataRef, const void*, void* info)
    {
        static_cast<Impl*>(info)->onClientReadable(cf);
    }

    ServerOptions options;
    OwningPointer<Dispatcher> dispatcher;
    CFSocketRef listenSocket = nullptr;
    CFRunLoopSourceRef listenSource = nullptr;
    RequestHandler handler;
    StreamingRouteFinder findStreamingRoute;
    int boundPort = -1;

    std::mutex streamingMutex;
    std::set<int> streamingFds;
    Vector<StreamingThread> streamingThreads;
    std::map<CFSocketRef, OwningPointer<Connection>> connections;
};

Server::Server(ServerOptions options)
    : impl(makeOwned<Impl>(options))
{
}

Server::~Server() = default;

bool Server::startListening(int port,
                            RequestHandler handler,
                            StreamingRouteFinder findStreamingRoute)
{
    return impl->start(port, std::move(handler), std::move(findStreamingRoute));
}

void Server::stop()
{
    impl->stop();
}

int Server::boundPort() const
{
    return impl->boundPort;
}

Server::Impl::~Impl()
{
    stop();
}

bool Server::Impl::start(int port, RequestHandler h, StreamingRouteFinder finder)
{
    if (listenSocket)
        return false;

    ignoreSigPipe();

    handler = std::move(h);
    findStreamingRoute = std::move(finder);

    auto context = CFSocketContext{0, this, nullptr, nullptr, nullptr};
    listenSocket = CFSocketCreate(kCFAllocatorDefault,
                                  PF_INET, SOCK_STREAM, IPPROTO_TCP,
                                  kCFSocketAcceptCallBack,
                                  &Impl::acceptCallback, &context);
    if (!listenSocket)
        return false;

    auto fd = CFSocketGetNative(listenSocket);
    auto yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    auto addr = sockaddr_in{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);
    addr.sin_addr.s_addr = bindAddress(options.bindTo);

    auto data = CFDataCreate(kCFAllocatorDefault,
                             (const UInt8*) &addr, sizeof(addr));
    auto err = CFSocketSetAddress(listenSocket, data);
    CFRelease(data);

    if (err != kCFSocketSuccess)
    {
        CFSocketInvalidate(listenSocket);
        CFRelease(listenSocket);
        listenSocket = nullptr;
        return false;
    }

    auto boundData = CFSocketCopyAddress(listenSocket);
    if (boundData)
    {
        auto* boundAddr = (const sockaddr_in*) CFDataGetBytePtr(boundData);
        boundPort = ntohs(boundAddr->sin_port);
        CFRelease(boundData);
    }

    listenSource = CFSocketCreateRunLoopSource(kCFAllocatorDefault,
                                               listenSocket, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), listenSource,
                       kCFRunLoopCommonModes);

    return true;
}

void Server::Impl::stop()
{
    if (dispatcher)
        dispatcher->shutdown();

    stopStreaming();

    for (auto& [cf, conn]: connections)
        detachClosingFd(*conn);
    connections.clear();

    if (listenSource)
    {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), listenSource,
                              kCFRunLoopCommonModes);
        CFRelease(listenSource);
        listenSource = nullptr;
    }
    if (listenSocket)
    {
        CFSocketInvalidate(listenSocket);
        CFRelease(listenSocket);
        listenSocket = nullptr;
    }
    boundPort = -1;
}

void Server::Impl::onAccept(CFSocketNativeHandle clientFd)
{
    auto conn = makeOwned<Connection>();
    readPeerEndpoint(clientFd, conn->remoteAddr, conn->remotePort);

    auto context = CFSocketContext{0, this, nullptr, nullptr, nullptr};
    conn->socket = CFSocketCreateWithNative(kCFAllocatorDefault, clientFd,
                                            kCFSocketReadCallBack,
                                            &Impl::clientCallback, &context);
    if (!conn->socket)
    {
        close(clientFd);
        return;
    }

    conn->source = CFSocketCreateRunLoopSource(kCFAllocatorDefault,
                                               conn->socket, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), conn->source,
                       kCFRunLoopCommonModes);

    auto cf = conn->socket;
    connections[cf] = std::move(conn);
}

void Server::Impl::detachKeepingFdOpen(Connection& conn)
{
    if (conn.source)
    {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), conn.source,
                              kCFRunLoopCommonModes);
        CFRelease(conn.source);
        conn.source = nullptr;
    }

    if (conn.socket)
    {
        auto flags = CFSocketGetSocketFlags(conn.socket);
        CFSocketSetSocketFlags(conn.socket, flags & ~kCFSocketCloseOnInvalidate);
        CFSocketInvalidate(conn.socket);
        CFRelease(conn.socket);
        conn.socket = nullptr;
    }
}

void Server::Impl::detachClosingFd(Connection& conn)
{
    if (conn.source)
    {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), conn.source,
                              kCFRunLoopCommonModes);
        CFRelease(conn.source);
        conn.source = nullptr;
    }

    if (conn.socket)
    {
        CFSocketInvalidate(conn.socket);
        CFRelease(conn.socket);
        conn.socket = nullptr;
    }
}

void Server::Impl::dispatchRequest(int fd, Request request)
{
    auto sendResponse = [fd](const Response& res)
    {
        writeResponseToFd(fd, res);
        ::close(fd);
    };

    dispatcher->dispatch(
        DispatchTask {std::move(request), handler, std::move(sendResponse)});
}

void Server::Impl::onClientReadable(CFSocketRef cf)
{
    auto it = connections.find(cf);
    if (it == connections.end())
        return;

    auto& conn = *it->second;
    auto fd = CFSocketGetNative(cf);

    char buf[4096];
    auto n = recv(fd, buf, sizeof(buf), 0);

    if (n <= 0)
    {
        closeConnection(cf);
        return;
    }

    auto state = conn.parser.feed(buf, (int) n);

    if (state == RequestParser::State::Invalid)
    {
        closeConnection(cf);
        return;
    }

    if (startStreamingIfRouted(conn, fd))
    {
        connections.erase(it);
        return;
    }

    if (state != RequestParser::State::Ready)
        return;

    auto request = std::move(conn.parser.request());
    request.remoteAddr = conn.remoteAddr;
    request.remotePort = conn.remotePort;

    detachKeepingFdOpen(conn);
    connections.erase(it);

    dispatchRequest(fd, std::move(request));
}

void Server::Impl::closeConnection(CFSocketRef cf)
{
    auto it = connections.find(cf);
    if (it == connections.end())
        return;

    detachClosingFd(*it->second);
    connections.erase(it);
}

// A streaming route blocks on its socket while the body arrives, which the
// main run loop these sockets live on cannot do, so the request moves to a
// thread of its own as soon as its headers are in.
bool Server::Impl::startStreamingIfRouted(Connection& conn, int fd)
{
    if (!conn.parser.hasHeaders() || conn.hasLookedForStreamingRoute)
        return false;

    conn.hasLookedForStreamingRoute = true;
    auto streamingHandler = findStreamingRoute(conn.parser.request());

    if (!streamingHandler.has_value())
        return false;

    auto head = std::move(conn.parser.request());
    head.body.clear();
    head.remoteAddr = conn.remoteAddr;
    head.remotePort = conn.remotePort;
    auto bytesAfterHeaders = conn.parser.takeBytesAfterHeaders();

    detachKeepingFdOpen(conn);
    startStreamingThread(
        fd, std::move(head), std::move(bytesAfterHeaders), std::move(*streamingHandler));
    return true;
}

void Server::Impl::startStreamingThread(int fd,
                                        Request head,
                                        std::string bytesAfterHeaders,
                                        StreamingRequestHandler streamingHandler)
{
    joinFinishedStreamingThreads();
    makeBlocking(fd);

    auto hasFinished = std::make_shared<std::atomic<bool>>(false);
    auto lock = std::lock_guard(streamingMutex);
    streamingFds.insert(fd);

    auto thread = std::thread(
        [this,
         fd,
         hasFinished,
         requestHead = std::move(head),
         leftoverBytes = std::move(bytesAfterHeaders),
         handler = std::move(streamingHandler)]() mutable
        {
            serveStreamingFd(fd, requestHead, std::move(leftoverBytes), handler);
            *hasFinished = true;
        });

    streamingThreads.add(StreamingThread {std::move(thread), hasFinished});
}

void Server::Impl::serveStreamingFd(int fd,
                                    const Request& head,
                                    std::string bytesAfterHeaders,
                                    const StreamingRequestHandler& streamingHandler)
{
    auto peer = PeerStream();
    peer.alreadyReceived = std::move(bytesAfterHeaders);
    peer.receiveMore = [fd] { return receiveFromFd(fd); };
    peer.send = [fd](std::string_view bytes) { sendAll(fd, bytes); };

    auto response = serveStreamingRequest(streamingHandler, head, std::move(peer));
    writeResponseToFd(fd, response);

    auto lock = std::lock_guard(streamingMutex);
    streamingFds.erase(fd);
    ::close(fd);
}

void Server::Impl::joinFinishedStreamingThreads()
{
    auto finished = Vector<StreamingThread>();
    {
        auto lock = std::lock_guard(streamingMutex);
        auto isFinished = [](const StreamingThread& t) { return t.hasFinished->load(); };

        for (auto& t: streamingThreads)
            if (isFinished(t))
                finished.add(std::move(t));

        streamingThreads.eraseIf([](const StreamingThread& t)
                                 { return !t.thread.joinable(); });
    }

    for (auto& t: finished)
        t.thread.join();
}

// Shutting a socket down wakes the recv its thread is blocked in, which then
// reads the body as ended early and replies to nobody.
void Server::Impl::stopStreaming()
{
    auto threadsToJoin = Vector<StreamingThread>();
    {
        auto lock = std::lock_guard(streamingMutex);

        for (auto fd: streamingFds)
            ::shutdown(fd, SHUT_RDWR);

        threadsToJoin = std::move(streamingThreads);
        streamingThreads.clear();
    }

    for (auto& t: threadsToJoin)
        t.thread.join();
}

} // namespace eacp::HTTP
