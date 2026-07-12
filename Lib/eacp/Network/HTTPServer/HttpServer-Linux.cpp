#include "HttpServer.h"
#include "../Common-Posix.h"
#include "HttpServerDispatcher.h"

#include "../HTTP/HttpProtocol.h"

#include <thread>

namespace eacp::HTTP
{

namespace
{

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
        auto n =
            ::send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);

        if (n <= 0)
            break;

        sent += (std::size_t) n;
    }
}

void writeResponseToFd(int fd, const Response& res)
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
    void handleConnection(int fd, const std::string& remoteAddr, int remotePort);
    void dispatchRequest(int fd, Request request);

    ServerOptions options;
    OwningPointer<Dispatcher> dispatcher;
    int listenSocket = -1;
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
    if (listenSocket != -1)
        return false;

    ignoreSigPipe();
    handler = std::move(h);

    listenSocket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket < 0)
    {
        listenSocket = -1;
        return false;
    }

    auto yes = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    auto addr = sockaddr_in();
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(listenSocket, (sockaddr*) &addr, sizeof(addr)) < 0)
    {
        ::close(listenSocket);
        listenSocket = -1;
        return false;
    }

    if (::listen(listenSocket, SOMAXCONN) < 0)
    {
        ::close(listenSocket);
        listenSocket = -1;
        return false;
    }

    auto bound = sockaddr_in();
    auto boundLen = (socklen_t) sizeof(bound);
    if (getsockname(listenSocket, (sockaddr*) &bound, &boundLen) == 0)
        boundPort = ntohs(bound.sin_port);

    running = true;
    acceptThread = std::thread([this] { acceptLoop(); });
    return true;
}

void Server::Impl::stop()
{
    running = false;

    if (listenSocket != -1)
    {
        ::shutdown(listenSocket, SHUT_RDWR);
        ::close(listenSocket);
        listenSocket = -1;
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
        auto addrLen = (socklen_t) sizeof(addr);
        auto clientSocket = ::accept(listenSocket, (sockaddr*) &addr, &addrLen);
        if (clientSocket < 0)
            break;

        auto remoteAddr = remoteAddressString(addr);
        auto remotePort = (int) ntohs(addr.sin_port);

        auto lock = std::lock_guard(clientMutex);
        clientThreads.emplace_back(
            [this, clientSocket, remoteAddr, remotePort]
            { handleConnection(clientSocket, remoteAddr, remotePort); });
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

void Server::Impl::handleConnection(int fd,
                                    const std::string& remoteAddr,
                                    int remotePort)
{
    auto parser = RequestParser();
    char buf[4096];

    while (true)
    {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            ::close(fd);
            return;
        }

        auto state = parser.feed(buf, (std::size_t) n);

        if (state == RequestParser::State::Invalid)
        {
            ::close(fd);
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
