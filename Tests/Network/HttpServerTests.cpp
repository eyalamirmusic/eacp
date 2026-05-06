#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Network/HTTP/Http.h>
#include <eacp/Network/HTTPServer/HttpServer.h>
#include <NanoTest/NanoTest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace nano;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::Threads::callAsync;
using eacp::Threads::stopEventLoop;

namespace
{
std::atomic<int> nextPort {52001};

int reservePort()
{
    return nextPort.fetch_add(1);
}

std::string baseUrl(int port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

struct Exchange
{
    Request received;
    bool handlerCalled = false;
    Response clientResponse;
    bool completed = false;
};

void performExchange(Server& server, const Request& clientRequest,
                     Exchange& out)
{
    auto worker = std::thread();

    auto stopped = eacp::Threads::runEventLoopFor(
        std::chrono::seconds(5),
        [&]
        {
            worker = std::thread([&]
            {
                out.clientResponse =
                    eacp::HTTP::httpRequest(clientRequest);
                callAsync([] { stopEventLoop(); });
            });
        });

    worker.join();
    out.completed = stopped;
    server.stop();
}
} // namespace

auto tListenSucceeds = test("HttpServer/listenReturnsTrueOnFreshPort") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ok = server.listen(port, [](const Request&) { return Response(); });
    check(ok);
};

auto tListenTwiceFails = test("HttpServer/listenTwiceOnSameInstanceFails") = []
{
    auto server = Server();
    auto port = reservePort();
    check(server.listen(port, [](const Request&) { return Response(); }));
    check(!server.listen(port, [](const Request&) { return Response(); }));
};

auto tStopAllowsRelisten = test("HttpServer/stopAllowsRelisten") = []
{
    auto server = Server();
    auto port = reservePort();
    check(server.listen(port, [](const Request&) { return Response(); }));
    server.stop();
    check(server.listen(port, [](const Request&) { return Response(); }));
};

auto tHandlerReceivesGet = test("HttpServer/handlerReceivesGetRequest") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port, [&](const Request& req)
    {
        ex.received = req;
        ex.handlerCalled = true;
        auto res = Response();
        res.statusCode = 200;
        res.content = "hello";
        return res;
    });
    check(ok);

    performExchange(server, Request(baseUrl(port) + "/ping"), ex);

    check(ex.completed);
    check(ex.handlerCalled);
    check(ex.received.type == "GET");
    check(ex.received.url == "/ping");
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.content == "hello");
};

auto tHandlerReceivesPostBody =
    test("HttpServer/handlerReceivesPostBody") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port, [&](const Request& req)
    {
        ex.received = req;
        ex.handlerCalled = true;
        auto res = Response();
        res.statusCode = 201;
        res.content = "created";
        return res;
    });
    check(ok);

    auto clientReq = Request::post(baseUrl(port) + "/items",
                                   "{\"name\":\"widget\"}");
    clientReq.headers["Content-Type"] = "application/json";

    performExchange(server, clientReq, ex);

    check(ex.completed);
    check(ex.received.type == "POST");
    check(ex.received.url == "/items");
    check(ex.received.body == "{\"name\":\"widget\"}");
    check(ex.clientResponse.statusCode == 201);
    check(ex.clientResponse.content == "created");
};

auto tHandlerReceivesHeaders =
    test("HttpServer/handlerReceivesCustomHeaders") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port, [&](const Request& req)
    {
        ex.received = req;
        ex.handlerCalled = true;
        auto res = Response();
        res.statusCode = 200;
        return res;
    });
    check(ok);

    auto clientReq = Request(baseUrl(port) + "/h");
    clientReq.headers["X-Custom-Header"] = "abc123";

    performExchange(server, clientReq, ex);

    check(ex.completed);
    check(ex.received.headers["X-Custom-Header"] == "abc123");
};

auto tResponseHeadersForwarded =
    test("HttpServer/responseHeadersAreForwardedToClient") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port, [&](const Request&)
    {
        auto res = Response();
        res.statusCode = 200;
        res.content = "{}";
        res.headers["Content-Type"] = "application/json";
        res.headers["X-Server-Tag"] = "eacp-test";
        return res;
    });
    check(ok);

    performExchange(server, Request(baseUrl(port) + "/json"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.content == "{}");
};

auto tDefaultStatusIs200 =
    test("HttpServer/responseWithoutStatusDefaultsTo200") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port, [&](const Request&)
    {
        auto res = Response();
        res.content = "ok";
        return res;
    });
    check(ok);

    performExchange(server, Request(baseUrl(port) + "/"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.content == "ok");
};

auto tNotFoundStatus = test("HttpServer/handlerCanReturnNotFound") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port, [&](const Request&)
    {
        auto res = Response();
        res.statusCode = 404;
        res.content = "missing";
        return res;
    });
    check(ok);

    performExchange(server, Request(baseUrl(port) + "/x"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 404);
    check(ex.clientResponse.content == "missing");
};
