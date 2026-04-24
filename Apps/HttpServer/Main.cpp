#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Network/HTTPServer/HttpServer.h>

#include <iostream>

eacp::HTTP::Response handle(const eacp::HTTP::Request& req)
{
    auto res = eacp::HTTP::Response();
    res.statusCode = 200;
    res.headers["Content-Type"] = "text/plain";
    res.content = req.type + " " + req.url + "\nbody: " + req.body + "\n";
    return res;
}

int main()
{
    auto server = eacp::HTTP::Server();
    auto ok = server.listen(8080, handle);

    if (!ok)
    {
        std::cerr << "Failed to listen on port 8080\n";
        return 1;
    }

    std::cout << "Listening on http://localhost:8080\n";
    eacp::Threads::runEventLoop();
    return 0;
}