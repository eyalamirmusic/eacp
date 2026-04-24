#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Server/HttpServer.h>

#include <iostream>

int main()
{
    auto server = eacp::HTTP::Server();

    auto ok = server.listen(8080,
                            [](const eacp::HTTP::Request& req)
                            {
                                auto res = eacp::HTTP::Response();
                                res.statusCode = 200;
                                res.headers["Content-Type"] = "text/plain";
                                res.content = req.type + " " + req.url
                                              + "\nbody: " + req.body + "\n";
                                return res;
                            });

    if (!ok)
    {
        std::cerr << "Failed to listen on port 8080\n";
        return 1;
    }

    std::cout << "Listening on http://localhost:8080\n";
    eacp::Threads::runEventLoop();
    return 0;
}