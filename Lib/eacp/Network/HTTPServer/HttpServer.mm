#include "HttpServer.h"
#include "HttpServerDispatcher.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Network/HTTP/HttpProtocol.h>

#include <eacp/Core/Utils/Containers.h>

#include <CoreFoundation/CoreFoundation.h>

#include <arpa/inet.h>
#include <csignal>
#include <map>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace eacp::HTTP
{

namespace
{

struct Connection
{
    CFSocketRef socket = nullptr;
    CFRunLoopSourceRef source = nullptr;
    RequestParser parser;
    std::string remoteAddr;
    int remotePort = -1;
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

void sendAll(int fd, const std::string& payload)
{
    auto sent = std::size_t {0};

    while (sent < payload.size())
    {
        auto n = ::send(fd, payload.data() + sent, payload.size() - sent, 0);

        if (n <= 0)
            break;

        sent += (std::size_t) n;
    }
}

void writeResponseToFd(int fd, const Response& res)
{
    sendAll(fd, serializeResponse(res));
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

} // namespace

struct Server::Impl
{
    explicit Impl(ServerOptions opts)
        : options(opts), dispatcher(makeDispatcher(opts))
    {
    }

    ~Impl();

    bool start(int port, RequestHandler h);
    void stop();

    void onAccept(CFSocketNativeHandle clientFd);
    void onClientReadable(CFSocketRef cf);
    void closeConnection(CFSocketRef cf);

    void detachKeepingFdOpen(Connection& conn);
    void detachClosingFd(Connection& conn);
    void dispatchRequest(int fd, Request request);

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
    int boundPort = -1;
    std::map<CFSocketRef, OwningPointer<Connection>> connections;
};

Server::Server(ServerOptions options)
    : impl(makeOwned<Impl>(options))
{
}

Server::~Server() = default;

bool Server::listen(int port, RequestHandler handler)
{
    return impl->start(port, std::move(handler));
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

bool Server::Impl::start(int port, RequestHandler h)
{
    if (listenSocket)
        return false;

    ignoreSigPipe();

    handler = std::move(h);

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
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

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

    auto state = conn.parser.feed(buf, (std::size_t) n);

    if (state == RequestParser::State::Invalid)
    {
        closeConnection(cf);
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

} // namespace eacp::HTTP
