#include "HttpServer.h"

#include <CoreFoundation/CoreFoundation.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <sstream>

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

struct Connection
{
    CFSocketRef socket = nullptr;
    CFRunLoopSourceRef source = nullptr;
    std::string buffer;
    bool headersParsed = false;
    size_t bodyStart = 0;
    size_t bodyExpected = 0;
    Request request;
};

} // namespace

struct Server::Impl
{
    CFSocketRef listenSocket = nullptr;
    CFRunLoopSourceRef listenSource = nullptr;
    RequestHandler handler;
    std::map<CFSocketRef, std::unique_ptr<Connection>> connections;

    ~Impl();

    bool start(int port, RequestHandler h);
    void stop();

    void onAccept(CFSocketNativeHandle clientFd);
    void onClientReadable(CFSocketRef cf);
    void closeConnection(CFSocketRef cf);
    void writeResponse(int fd, const Response& res);

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
    if (listenSocket)
        return false;

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

    listenSource = CFSocketCreateRunLoopSource(kCFAllocatorDefault,
                                               listenSocket, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), listenSource,
                       kCFRunLoopCommonModes);

    return true;
}

void Server::Impl::stop()
{
    for (auto& [cf, conn]: connections)
    {
        if (conn->source)
        {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), conn->source,
                                  kCFRunLoopCommonModes);
            CFRelease(conn->source);
        }
        if (conn->socket)
        {
            CFSocketInvalidate(conn->socket);
            CFRelease(conn->socket);
        }
    }
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
}

void Server::Impl::onAccept(CFSocketNativeHandle clientFd)
{
    auto conn = std::make_unique<Connection>();

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

    conn.buffer.append(buf, (size_t) n);

    if (!conn.headersParsed)
    {
        auto end = conn.buffer.find("\r\n\r\n");
        if (end == std::string::npos)
            return;

        auto stream = std::stringstream(conn.buffer.substr(0, end));
        auto line = std::string();

        if (!std::getline(stream, line))
        {
            closeConnection(cf);
            return;
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        auto sp1 = line.find(' ');
        auto sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos)
        {
            closeConnection(cf);
            return;
        }

        conn.request.type = line.substr(0, sp1);
        conn.request.url = line.substr(sp1 + 1, sp2 - sp1 - 1);

        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                break;
            auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            conn.request.headers[trim(line.substr(0, colon))] =
                trim(line.substr(colon + 1));
        }

        conn.bodyStart = end + 4;
        conn.headersParsed = true;

        for (auto& [k, v]: conn.request.headers)
        {
            if (toLower(k) == "content-length")
            {
                try
                {
                    conn.bodyExpected = (size_t) std::stoul(v);
                }
                catch (...)
                {
                }
                break;
            }
        }
    }

    if (conn.headersParsed
        && conn.buffer.size() - conn.bodyStart >= conn.bodyExpected)
    {
        conn.request.body = conn.buffer.substr(conn.bodyStart,
                                               conn.bodyExpected);

        auto response = handler ? handler(conn.request) : Response();
        writeResponse(fd, response);
        closeConnection(cf);
    }
}

void Server::Impl::closeConnection(CFSocketRef cf)
{
    auto it = connections.find(cf);
    if (it == connections.end())
        return;

    auto& conn = *it->second;
    if (conn.source)
    {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), conn.source,
                              kCFRunLoopCommonModes);
        CFRelease(conn.source);
    }
    if (conn.socket)
    {
        CFSocketInvalidate(conn.socket);
        CFRelease(conn.socket);
    }
    connections.erase(it);
}

void Server::Impl::writeResponse(int fd, const Response& res)
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
    auto sent = size_t{0};
    while (sent < out.size())
    {
        auto n = ::send(fd, out.data() + sent, out.size() - sent, 0);
        if (n <= 0)
            break;
        sent += (size_t) n;
    }
}

} // namespace eacp::HTTP
