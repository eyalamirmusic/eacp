#include <eacp/Core/Utils/WinInclude.h>

#include "HttpServer.h"
#include "HttpServerDispatcher.h"

#include <eacp/Network/HTTP/HttpProtocol.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <eacp/Core/Utils/Containers.h>
#include <mutex>
#include <string>
#include <thread>

namespace eacp::HTTP
{

namespace
{

struct WinsockInit
{
    WinsockInit()
    {
        auto wsa = WSADATA();
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockInit() { WSACleanup(); }
};

void ensureWinsockInitialized()
{
    static auto init = WinsockInit();
    (void) init;
}

void sendAll(SOCKET fd, const std::string& payload)
{
    auto sent = std::size_t {0};

    while (sent < payload.size())
    {
        auto n = ::send(fd, payload.data() + sent, (int) (payload.size() - sent), 0);

        if (n <= 0)
            break;

        sent += (std::size_t) n;
    }
}

void writeResponseToFd(SOCKET fd, const Response& res)
{
    sendAll(fd, serializeResponse(res));
}

std::string remoteAddressString(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return ip;
}

} // namespace

struct Server::Impl
{
    explicit Impl(ServerOptions opts)
        : options(opts)
        , dispatcher(makeDispatcher(opts))
    {
    }

    ~Impl();

    bool start(int port, RequestHandler h);
    void stop();

    void acceptLoop();
    void handleConnection(SOCKET clientSocket,
                          const std::string& remoteAddr,
                          int remotePort);
    void dispatchRequest(SOCKET fd, Request request);

    ServerOptions options;
    OwningPointer<Dispatcher> dispatcher;
    SOCKET listenSocket = INVALID_SOCKET;
    int boundPort = -1;
    RequestHandler handler;
    std::thread acceptThread;
    std::atomic<bool> running {false};

    std::mutex clientMutex;
    Vector<std::thread> clientThreads;
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
    if (listenSocket != INVALID_SOCKET)
        return false;

    ensureWinsockInitialized();
    handler = std::move(h);

    listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET)
        return false;

    auto yes = 1;
    setsockopt(
        listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*) &yes, sizeof(yes));

    auto addr = sockaddr_in();
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short) port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(listenSocket, (sockaddr*) &addr, sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
        return false;
    }

    if (::listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
        return false;
    }

    auto bound = sockaddr_in();
    auto boundLen = (int) sizeof(bound);
    if (getsockname(listenSocket, (sockaddr*) &bound, &boundLen) == 0)
        boundPort = ntohs(bound.sin_port);

    running = true;
    acceptThread = std::thread([this] { acceptLoop(); });
    return true;
}

void Server::Impl::stop()
{
    running = false;

    if (listenSocket != INVALID_SOCKET)
    {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }
    boundPort = -1;

    if (acceptThread.joinable())
        acceptThread.join();

    auto threadsToJoin = Vector<std::thread>();
    {
        auto lock = std::lock_guard(clientMutex);
        threadsToJoin = std::move(clientThreads);
    }
    for (auto& t: threadsToJoin)
        if (t.joinable())
            t.join();

    if (dispatcher)
        dispatcher->shutdown();
}

void Server::Impl::acceptLoop()
{
    while (running)
    {
        auto addr = sockaddr_in();
        auto addrLen = (int) sizeof(addr);
        auto clientSocket = ::accept(listenSocket, (sockaddr*) &addr, &addrLen);
        if (clientSocket == INVALID_SOCKET)
            break;

        auto remoteAddr = remoteAddressString(addr);
        auto remotePort = (int) ntohs(addr.sin_port);

        auto lock = std::lock_guard(clientMutex);
        clientThreads.emplace_back(
            [this, clientSocket, remoteAddr, remotePort]
            { handleConnection(clientSocket, remoteAddr, remotePort); });
    }
}

void Server::Impl::dispatchRequest(SOCKET fd, Request request)
{
    auto sendResponse = [fd](const Response& res)
    {
        writeResponseToFd(fd, res);
        closesocket(fd);
    };

    dispatcher->dispatch(
        DispatchTask {std::move(request), handler, std::move(sendResponse)});
}

void Server::Impl::handleConnection(SOCKET fd,
                                    const std::string& remoteAddr,
                                    int remotePort)
{
    auto parser = RequestParser();
    char buf[4096];

    while (true)
    {
        auto n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            closesocket(fd);
            return;
        }

        auto state = parser.feed(buf, (std::size_t) n);

        if (state == RequestParser::State::Invalid)
        {
            closesocket(fd);
            return;
        }

        if (state == RequestParser::State::Ready)
        {
            auto request = std::move(parser.request());
            request.remoteAddr = remoteAddr;
            request.remotePort = remotePort;
            dispatchRequest(fd, std::move(request));
            return;
        }
    }
}

} // namespace eacp::HTTP
