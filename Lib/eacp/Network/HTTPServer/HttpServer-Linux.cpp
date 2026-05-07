#include "HttpServer.h"
#include "HttpServerDispatcher.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <csignal>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace eacp::HTTP
{

namespace
{

std::string trim(const std::string& s)
{
    auto begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return "";
    auto end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

std::string toLower(std::string s)
{
    for (auto& c: s)
        c = (char) std::tolower((unsigned char) c);
    return s;
}

const char* reasonPhrase(int code)
{
    switch (code)
    {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 500:
            return "Internal Server Error";
        default:
            return "OK";
    }
}

void ignoreSigPipe()
{
    static auto once = []
    {
        ::signal(SIGPIPE, SIG_IGN);
        return 0;
    }();
    (void) once;
}

void writeResponseToFd(int fd, const Response& res)
{
    auto code = res.statusCode != 0 ? res.statusCode : 200;
    auto ss = std::stringstream();
    ss << "HTTP/1.1 " << code << " " << reasonPhrase(code) << "\r\n";

    auto hasContentLength = false;
    for (auto& [k, v]: res.headers)
    {
        if (toLower(k) == "content-length")
            hasContentLength = true;
        ss << k << ": " << v << "\r\n";
    }
    if (!hasContentLength)
        ss << "Content-Length: " << res.content.size() << "\r\n";
    ss << "Connection: close\r\n\r\n";
    ss << res.content;

    auto out = ss.str();
    auto sent = size_t {0};
    while (sent < out.size())
    {
        auto n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
            break;
        sent += (size_t) n;
    }
}

} // namespace

struct Server::Impl
{
    explicit Impl(ServerOptions opts)
        : options(opts), dispatcher(makeDispatcher(opts))
    {
    }

    ServerOptions options;
    std::unique_ptr<Dispatcher> dispatcher;
    int listenSocket = -1;
    RequestHandler handler;
    std::thread acceptThread;
    std::atomic<bool> running {false};

    std::mutex clientMutex;
    std::vector<std::thread> clientThreads;

    ~Impl();

    bool start(int port, RequestHandler h);
    void stop();

    void acceptLoop();
    void handleConnection(int fd, const std::string& remoteAddr, int remotePort);
};

Server::Server(ServerOptions options)
    : impl(std::make_unique<Impl>(options))
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

    if (acceptThread.joinable())
        acceptThread.join();

    auto threadsToJoin = std::vector<std::thread>();
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

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        auto remoteAddr = std::string(ip);
        auto remotePort = (int) ntohs(addr.sin_port);

        auto lock = std::lock_guard(clientMutex);
        clientThreads.emplace_back(
            [this, clientSocket, remoteAddr, remotePort]
            { handleConnection(clientSocket, remoteAddr, remotePort); });
    }
}

void Server::Impl::handleConnection(int fd,
                                    const std::string& remoteAddr,
                                    int remotePort)
{
    auto buffer = std::string();
    auto headersParsed = false;
    auto bodyStart = size_t {0};
    auto bodyExpected = size_t {0};
    auto request = Request();
    request.remoteAddr = remoteAddr;
    request.remotePort = remotePort;

    char buf[4096];
    while (true)
    {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            ::close(fd);
            return;
        }

        buffer.append(buf, (size_t) n);

        if (!headersParsed)
        {
            auto end = buffer.find("\r\n\r\n");
            if (end == std::string::npos)
                continue;

            auto stream = std::stringstream(buffer.substr(0, end));
            auto line = std::string();

            if (!std::getline(stream, line))
            {
                ::close(fd);
                return;
            }
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            auto sp1 = line.find(' ');
            auto sp2 = line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos)
            {
                ::close(fd);
                return;
            }

            request.type = line.substr(0, sp1);
            request.url = line.substr(sp1 + 1, sp2 - sp1 - 1);

            auto query = request.url.find('?');
            if (query != std::string::npos)
                request.params = parseQueryString(request.url.substr(query + 1));

            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    break;
                auto colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                request.headers[trim(line.substr(0, colon))] =
                    trim(line.substr(colon + 1));
            }

            bodyStart = end + 4;
            headersParsed = true;

            for (auto& [k, v]: request.headers)
            {
                if (toLower(k) == "content-length")
                {
                    try
                    {
                        bodyExpected = (size_t) std::stoul(v);
                    }
                    catch (...)
                    {
                    }
                    break;
                }
            }
        }

        if (headersParsed && buffer.size() - bodyStart >= bodyExpected)
        {
            request.body = buffer.substr(bodyStart, bodyExpected);

            auto sendResponse = [fd](const Response& res)
            {
                writeResponseToFd(fd, res);
                ::close(fd);
            };

            dispatcher->dispatch(DispatchTask {
                std::move(request), handler, std::move(sendResponse)});
            return;
        }
    }
}

} // namespace eacp::HTTP
