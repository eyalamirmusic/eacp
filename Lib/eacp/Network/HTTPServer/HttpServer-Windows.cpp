#include "HttpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
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
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

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

} // namespace

struct Server::Impl
{
    SOCKET listenSocket = INVALID_SOCKET;
    RequestHandler handler;
    std::thread acceptThread;
    std::atomic<bool> running {false};

    std::mutex clientMutex;
    std::vector<std::thread> clientThreads;

    ~Impl();

    bool start(int port, RequestHandler h);
    void stop();

    void acceptLoop();
    void handleConnection(SOCKET clientSocket);
    void writeResponse(SOCKET fd, const Response& res);
};

Server::Server()
    : impl(std::make_unique<Impl>())
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
    if (listenSocket != INVALID_SOCKET)
        return false;

    ensureWinsockInitialized();
    handler = std::move(h);

    listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET)
        return false;

    auto yes = 1;
    setsockopt(listenSocket,
               SOL_SOCKET,
               SO_REUSEADDR,
               (const char*) &yes,
               sizeof(yes));

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
}

void Server::Impl::acceptLoop()
{
    while (running)
    {
        auto clientSocket = ::accept(listenSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET)
            break;

        auto lock = std::lock_guard(clientMutex);
        clientThreads.emplace_back([this, clientSocket]
                                   { handleConnection(clientSocket); });
    }
}

void Server::Impl::handleConnection(SOCKET fd)
{
    auto buffer = std::string();
    auto headersParsed = false;
    auto bodyStart = size_t {0};
    auto bodyExpected = size_t {0};
    auto request = Request();

    char buf[4096];
    while (true)
    {
        auto n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            closesocket(fd);
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
                closesocket(fd);
                return;
            }
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            auto sp1 = line.find(' ');
            auto sp2 = line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos)
            {
                closesocket(fd);
                return;
            }

            request.type = line.substr(0, sp1);
            request.url = line.substr(sp1 + 1, sp2 - sp1 - 1);

            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    break;
                auto colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                request.headers[trim(line.substr(0, colon))]
                    = trim(line.substr(colon + 1));
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

            auto response = handler ? handler(request) : Response();
            writeResponse(fd, response);
            closesocket(fd);
            return;
        }
    }
}

void Server::Impl::writeResponse(SOCKET fd, const Response& res)
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
        auto n = ::send(fd,
                        out.data() + sent,
                        (int) (out.size() - sent),
                        0);
        if (n <= 0)
            break;
        sent += (size_t) n;
    }
}

} // namespace eacp::HTTP
