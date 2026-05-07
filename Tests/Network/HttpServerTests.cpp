#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Network/HTTP/Http.h>
#include <eacp/Network/HTTPServer/HttpServer.h>
#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace nano;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::HTTP::ServerOptions;
using eacp::HTTP::ServerThreadingMode;
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

void performExchange(Server& server, const Request& clientRequest, Exchange& out)
{
    auto worker = std::thread();

    auto stopped = eacp::Threads::runEventLoopFor(
        std::chrono::seconds(5),
        [&]
        {
            worker = std::thread(
                [&]
                {
                    out.clientResponse = eacp::HTTP::httpRequest(clientRequest);
                    callAsync([] { stopEventLoop(); });
                });
        });

    worker.join();
    out.completed = stopped;
    server.stop();
}

struct ParallelExchange
{
    std::vector<Response> responses;
    bool completed = false;
};

void performParallelExchange(Server& server,
                             const std::vector<Request>& requests,
                             ParallelExchange& out,
                             std::chrono::milliseconds timeout
                             = std::chrono::seconds(10))
{
    auto n = requests.size();
    out.responses.assign(n, Response());

    auto remaining = std::make_shared<std::atomic<int>>((int) n);
    auto workers = std::vector<std::thread>();
    workers.reserve(n);

    auto stopped = eacp::Threads::runEventLoopFor(
        timeout,
        [&]
        {
            for (auto i = size_t {0}; i < n; ++i)
            {
                workers.emplace_back(
                    [&, i]
                    {
                        out.responses[i] =
                            eacp::HTTP::httpRequest(requests[i]);
                        if (remaining->fetch_sub(1) == 1)
                            callAsync([] { stopEventLoop(); });
                    });
            }
        });

    for (auto& w: workers)
        if (w.joinable())
            w.join();

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

    auto ok = server.listen(port,
                            [&](const Request& req)
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

auto tHandlerReceivesPostBody = test("HttpServer/handlerReceivesPostBody") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port,
                            [&](const Request& req)
                            {
                                ex.received = req;
                                ex.handlerCalled = true;
                                auto res = Response();
                                res.statusCode = 201;
                                res.content = "created";
                                return res;
                            });
    check(ok);

    auto clientReq =
        Request::post(baseUrl(port) + "/items", "{\"name\":\"widget\"}");
    clientReq.headers["Content-Type"] = "application/json";

    performExchange(server, clientReq, ex);

    check(ex.completed);
    check(ex.received.type == "POST");
    check(ex.received.url == "/items");
    check(ex.received.body == "{\"name\":\"widget\"}");
    check(ex.clientResponse.statusCode == 201);
    check(ex.clientResponse.content == "created");
};

auto tHandlerReceivesHeaders = test("HttpServer/handlerReceivesCustomHeaders") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port,
                            [&](const Request& req)
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

    auto ok = server.listen(port,
                            [&](const Request&)
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

auto tDefaultStatusIs200 = test("HttpServer/responseWithoutStatusDefaultsTo200") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port,
                            [&](const Request&)
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

    auto ok = server.listen(port,
                            [&](const Request&)
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

auto tHandlerReceivesQueryParams =
    test("HttpServer/handlerReceivesParsedQueryParams") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port,
                            [&](const Request& req)
                            {
                                ex.received = req;
                                ex.handlerCalled = true;
                                return Response();
                            });
    check(ok);

    performExchange(
        server, Request(baseUrl(port) + "/search?q=hello%20world&limit=10"), ex);

    check(ex.completed);
    check(ex.handlerCalled);
    check(ex.received.params.size() == 2);
    check(ex.received.params["q"] == "hello world");
    check(ex.received.params["limit"] == "10");
};

auto tHandlerReceivesEmptyParamsForNoQuery =
    test("HttpServer/handlerHasEmptyParamsWhenNoQuery") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port,
                            [&](const Request& req)
                            {
                                ex.received = req;
                                ex.handlerCalled = true;
                                return Response();
                            });
    check(ok);

    performExchange(server, Request(baseUrl(port) + "/plain"), ex);

    check(ex.completed);
    check(ex.handlerCalled);
    check(ex.received.params.empty());
};

auto tHandlerReceivesRemoteAddr =
    test("HttpServer/handlerReceivesRemoteAddrAndPort") = []
{
    auto server = Server();
    auto port = reservePort();
    auto ex = Exchange();

    auto ok = server.listen(port,
                            [&](const Request& req)
                            {
                                ex.received = req;
                                ex.handlerCalled = true;
                                return Response();
                            });
    check(ok);

    performExchange(server, Request(baseUrl(port) + "/who"), ex);

    check(ex.completed);
    check(ex.handlerCalled);
    check(ex.received.remoteAddr == "127.0.0.1");
    check(ex.received.remotePort > 0);
    check(ex.received.remotePort != port);
};

auto tEventLoopModeSerializesHandlers =
    test("HttpServer/eventLoopModeSerializesHandlerInvocations") = []
{
    auto opts = ServerOptions {};
    opts.threading = ServerThreadingMode::EventLoop;
    auto server = Server(opts);
    auto port = reservePort();

    auto inFlight = std::atomic<int> {0};
    auto maxInFlight = std::atomic<int> {0};

    auto ok = server.listen(
        port,
        [&](const Request&)
        {
            auto cur = inFlight.fetch_add(1) + 1;
            auto m = maxInFlight.load();
            while (cur > m && !maxInFlight.compare_exchange_weak(m, cur))
            {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            inFlight.fetch_sub(1);

            auto res = Response();
            res.statusCode = 200;
            res.content = "ok";
            return res;
        });
    check(ok);

    auto requests = std::vector<Request>();
    for (auto i = 0; i < 4; ++i)
        requests.emplace_back(baseUrl(port) + "/p");

    auto out = ParallelExchange();
    performParallelExchange(server, requests, out);

    check(out.completed);
    check(maxInFlight.load() == 1);
    check(out.responses.size() == 4);
    for (auto& r: out.responses)
    {
        check(r.statusCode == 200);
        check(r.content == "ok");
    }
};

auto tThreadPoolModeRunsHandlersInParallel =
    test("HttpServer/threadPoolModeRunsHandlersInParallel") = []
{
    auto opts = ServerOptions {};
    opts.threading = ServerThreadingMode::ThreadPool;
    opts.threadPoolSize = 4;
    auto server = Server(opts);
    auto port = reservePort();

    auto barrierCount = std::atomic<int> {0};
    auto allArrived = std::atomic<bool> {false};

    auto ok = server.listen(
        port,
        [&](const Request&)
        {
            barrierCount.fetch_add(1);

            auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (barrierCount.load() < 4
                   && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));

            if (barrierCount.load() >= 4)
                allArrived.store(true);

            auto res = Response();
            res.statusCode = 200;
            res.content = "ok";
            return res;
        });
    check(ok);

    auto requests = std::vector<Request>();
    for (auto i = 0; i < 4; ++i)
        requests.emplace_back(baseUrl(port) + "/q");

    auto out = ParallelExchange();
    performParallelExchange(server, requests, out);

    check(out.completed);
    check(allArrived.load());
    check(out.responses.size() == 4);
    for (auto& r: out.responses)
    {
        check(r.statusCode == 200);
        check(r.content == "ok");
    }
};

auto tThreadPoolModeAssignsDistinctRemotePorts =
    test("HttpServer/threadPoolModeAssignsDistinctRemotePortsPerClient") = []
{
    auto opts = ServerOptions {};
    opts.threading = ServerThreadingMode::ThreadPool;
    opts.threadPoolSize = 4;
    auto server = Server(opts);
    auto port = reservePort();

    auto remotePortsMutex = std::mutex();
    auto remotePorts = std::vector<int>();

    auto ok = server.listen(
        port,
        [&](const Request& req)
        {
            {
                auto lock = std::lock_guard(remotePortsMutex);
                remotePorts.push_back(req.remotePort);
            }
            auto res = Response();
            res.statusCode = 200;
            res.content = "ok";
            return res;
        });
    check(ok);

    auto requests = std::vector<Request>();
    for (auto i = 0; i < 6; ++i)
        requests.emplace_back(baseUrl(port) + "/multi");

    auto out = ParallelExchange();
    performParallelExchange(server, requests, out);

    check(out.completed);
    check(out.responses.size() == 6);
    for (auto& r: out.responses)
        check(r.statusCode == 200);

    auto lock = std::lock_guard(remotePortsMutex);
    check(remotePorts.size() == 6);
    for (auto p: remotePorts)
        check(p > 0 && p != port);
    auto sorted = remotePorts;
    std::sort(sorted.begin(), sorted.end());
    check(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
};
