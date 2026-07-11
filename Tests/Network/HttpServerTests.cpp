#include "Common.h"
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

void performParallelExchange(
    Server& server,
    const std::vector<Request>& requests,
    ParallelExchange& out,
    std::chrono::milliseconds timeout = std::chrono::seconds(10))
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
                        out.responses[i] = eacp::HTTP::httpRequest(requests[i]);
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
    auto port = 0;
    auto ok = server.listen(0, [](const Request&) { return Response(); });
    check(ok);
    port = server.boundPort();
};

auto tListenTwiceFails = test("HttpServer/listenTwiceOnSameInstanceFails") = []
{
    auto server = Server();
    auto port = 0;
    check(server.listen(0, [](const Request&) { return Response(); }));
    check(!server.listen(0, [](const Request&) { return Response(); }));
};

auto tStopAllowsRelisten = test("HttpServer/stopAllowsRelisten") = []
{
    auto server = Server();
    auto port = 0;
    check(server.listen(0, [](const Request&) { return Response(); }));
    server.stop();
    check(server.listen(0, [](const Request&) { return Response(); }));
};

auto tHandlerReceivesGet = test("HttpServer/handlerReceivesGetRequest") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    auto ok = server.listen(0,
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
    port = server.boundPort();

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
    auto port = 0;
    auto ex = Exchange();

    auto ok = server.listen(0,
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
    port = server.boundPort();

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
    auto port = 0;
    auto ex = Exchange();

    auto ok = server.listen(0,
                            [&](const Request& req)
                            {
                                ex.received = req;
                                ex.handlerCalled = true;
                                auto res = Response();
                                res.statusCode = 200;
                                return res;
                            });
    check(ok);
    port = server.boundPort();

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
    auto port = 0;
    auto ex = Exchange();

    auto ok = server.listen(0,
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
    port = server.boundPort();

    performExchange(server, Request(baseUrl(port) + "/json"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.content == "{}");
    check(ex.clientResponse.headers["Content-Type"] == "application/json");
    check(ex.clientResponse.headers["X-Server-Tag"] == "eacp-test");
};

auto tMultipleResponseHeadersRoundTrip =
    test("HttpServer/multipleResponseHeadersRoundTripToClient") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    auto ok = server.listen(0,
                            [&](const Request&)
                            {
                                auto res = Response();
                                res.statusCode = 200;
                                res.headers["X-Trace-Id"] = "abc-123";
                                res.headers["X-Build"] = "v42";
                                res.headers["Cache-Control"] = "no-store";
                                return res;
                            });
    check(ok);
    port = server.boundPort();

    performExchange(server, Request(baseUrl(port) + "/multi"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.headers["X-Trace-Id"] == "abc-123");
    check(ex.clientResponse.headers["X-Build"] == "v42");
    check(ex.clientResponse.headers["Cache-Control"] == "no-store");
};

auto tDefaultStatusIs200 = test("HttpServer/responseWithoutStatusDefaultsTo200") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    auto ok = server.listen(0,
                            [&](const Request&)
                            {
                                auto res = Response();
                                res.content = "ok";
                                return res;
                            });
    check(ok);
    port = server.boundPort();

    performExchange(server, Request(baseUrl(port) + "/"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.content == "ok");
};

auto tNotFoundStatus = test("HttpServer/handlerCanReturnNotFound") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    auto ok = server.listen(0,
                            [&](const Request&)
                            {
                                auto res = Response();
                                res.statusCode = 404;
                                res.content = "missing";
                                return res;
                            });
    check(ok);
    port = server.boundPort();

    performExchange(server, Request(baseUrl(port) + "/x"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 404);
    check(ex.clientResponse.content == "missing");
};

auto tHandlerReceivesQueryParams =
    test("HttpServer/handlerReceivesParsedQueryParams") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    auto ok = server.listen(0,
                            [&](const Request& req)
                            {
                                ex.received = req;
                                ex.handlerCalled = true;
                                return Response();
                            });
    check(ok);
    port = server.boundPort();

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
    auto port = 0;
    auto ex = Exchange();

    auto ok = server.listen(0,
                            [&](const Request& req)
                            {
                                ex.received = req;
                                ex.handlerCalled = true;
                                return Response();
                            });
    check(ok);
    port = server.boundPort();

    performExchange(server, Request(baseUrl(port) + "/plain"), ex);

    check(ex.completed);
    check(ex.handlerCalled);
    check(ex.received.params.empty());
};

auto tHandlerReceivesRemoteAddr =
    test("HttpServer/handlerReceivesRemoteAddrAndPort") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    auto ok = server.listen(0,
                            [&](const Request& req)
                            {
                                ex.received = req;
                                ex.handlerCalled = true;
                                return Response();
                            });
    check(ok);
    port = server.boundPort();

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
    auto port = 0;

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
    port = server.boundPort();

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
    auto port = 0;

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
    port = server.boundPort();

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
    auto port = 0;

    auto remotePortsMutex = std::mutex();
    auto remotePorts = std::vector<int>();

    auto ok = server.listen(0,
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
    port = server.boundPort();

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

using eacp::HTTP::Error;
using eacp::HTTP::throwError;

auto tErrorStatusAndMessage =
    test("HttpError/constructorFromStatusAndMessageBuildsPlainTextResponse") = []
{
    auto err = Error(418, "I'm a teapot");
    check(err.statusCode == 418);
    check(std::string(err.what()) == "I'm a teapot");
    check(err.response.statusCode == 418);
    check(err.response.content == "I'm a teapot");
    check(err.response.headers["Content-Type"] == "text/plain");
};

auto tErrorFromResponse = test("HttpError/constructorFromResponsePreservesIt") = []
{
    auto res = Response();
    res.statusCode = 422;
    res.content = "{\"x\":1}";
    res.headers["Content-Type"] = "application/json";

    auto err = Error(res);
    check(err.statusCode == 422);
    check(err.response.statusCode == 422);
    check(err.response.content == "{\"x\":1}");
    check(err.response.headers["Content-Type"] == "application/json");
};

auto tThrowErrorThrowsErrorWithStatus =
    test("HttpError/throwErrorThrowsErrorCarryingStatus") = []
{
    auto caught = false;
    try
    {
        throwError("nope", 401);
    }
    catch (const Error& e)
    {
        caught = true;
        check(e.statusCode == 401);
        check(std::string(e.what()) == "nope");
    }
    check(caught);
};

auto tThrowErrorDefaultStatusIs400 = test("HttpError/throwErrorDefaultsTo400") = []
{
    auto caught = false;
    try
    {
        throwError("bad");
    }
    catch (const Error& e)
    {
        caught = true;
        check(e.statusCode == 400);
    }
    check(caught);
};

auto tRouterDispatchesGet = test("HttpServer/routerDispatchesGetRoute") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    server.get("/ping",
               [&](const Request& req)
               {
                   ex.received = req;
                   ex.handlerCalled = true;
                   auto res = Response();
                   res.statusCode = 200;
                   res.content = "pong";
                   return res;
               });

    check(server.listen(0));
    port = server.boundPort();
    performExchange(server, Request(baseUrl(port) + "/ping"), ex);

    check(ex.completed);
    check(ex.handlerCalled);
    check(ex.received.type == "GET");
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.content == "pong");
};

auto tRouterDispatchesPost = test("HttpServer/routerDispatchesPostRoute") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    server.post("/items",
                [&](const Request& req)
                {
                    ex.received = req;
                    ex.handlerCalled = true;
                    auto res = Response();
                    res.statusCode = 201;
                    res.content = "made";
                    return res;
                });

    check(server.listen(0));
    port = server.boundPort();
    performExchange(server, Request::post(baseUrl(port) + "/items", "body"), ex);

    check(ex.completed);
    check(ex.received.type == "POST");
    check(ex.received.body == "body");
    check(ex.clientResponse.statusCode == 201);
    check(ex.clientResponse.content == "made");
};

auto tRouterMethodAndPathAreBothMatched =
    test("HttpServer/routerSeparatesByMethodAndPath") = []
{
    auto server = Server();
    auto port = 0;

    auto getCalls = std::atomic<int> {0};
    auto postCalls = std::atomic<int> {0};

    server.get("/x",
               [&](const Request&)
               {
                   getCalls.fetch_add(1);
                   auto res = Response();
                   res.statusCode = 200;
                   res.content = "g";
                   return res;
               });
    server.post("/x",
                [&](const Request&)
                {
                    postCalls.fetch_add(1);
                    auto res = Response();
                    res.statusCode = 200;
                    res.content = "p";
                    return res;
                });

    check(server.listen(0));
    port = server.boundPort();

    auto requests = std::vector<Request>();
    requests.emplace_back(baseUrl(port) + "/x");
    requests.emplace_back(Request::post(baseUrl(port) + "/x", ""));

    auto out = ParallelExchange();
    performParallelExchange(server, requests, out);

    check(out.completed);
    check(getCalls.load() == 1);
    check(postCalls.load() == 1);

    auto bodies = std::vector<std::string>();
    for (auto& r: out.responses)
        bodies.push_back(r.content);
    std::sort(bodies.begin(), bodies.end());
    check(bodies[0] == "g");
    check(bodies[1] == "p");
};

auto tRouterUnknownReturns404 = test("HttpServer/routerReturns404WhenNoMatch") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    server.get("/exists",
               [](const Request&)
               {
                   auto r = Response();
                   r.statusCode = 200;
                   return r;
               });

    check(server.listen(0));
    port = server.boundPort();
    performExchange(server, Request(baseUrl(port) + "/missing"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 404);
    check(ex.clientResponse.content == "Not Found");
};

auto tRouterStripsQueryString =
    test("HttpServer/routerMatchesPathIgnoringQueryString") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    server.get("/search",
               [&](const Request& req)
               {
                   ex.received = req;
                   ex.handlerCalled = true;
                   auto res = Response();
                   res.statusCode = 200;
                   res.content = "ok";
                   return res;
               });

    check(server.listen(0));
    port = server.boundPort();
    performExchange(server, Request(baseUrl(port) + "/search?q=cat&limit=5"), ex);

    check(ex.completed);
    check(ex.handlerCalled);
    check(ex.received.params["q"] == "cat");
    check(ex.received.params["limit"] == "5");
    check(ex.clientResponse.statusCode == 200);
};

auto tRouterCatchesError = test("HttpServer/routerConvertsErrorToItsResponse") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    server.get("/fail",
               [](const Request&) -> Response { throwError("not allowed", 403); });

    check(server.listen(0));
    port = server.boundPort();
    performExchange(server, Request(baseUrl(port) + "/fail"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 403);
    check(ex.clientResponse.content == "not allowed");
};

auto tRouterCatchesErrorWithCustomResponse =
    test("HttpServer/routerForwardsErrorPrebuiltResponse") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    server.get("/fail",
               [](const Request&) -> Response
               {
                   auto res = Response();
                   res.statusCode = 451;
                   res.content = "{\"why\":\"legal\"}";
                   res.headers["Content-Type"] = "application/json";
                   throw Error(std::move(res));
               });

    check(server.listen(0));
    port = server.boundPort();
    performExchange(server, Request(baseUrl(port) + "/fail"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 451);
    check(ex.clientResponse.content == "{\"why\":\"legal\"}");
};

auto tRouterCatchesUnknownExceptionAs500 =
    test("HttpServer/routerConvertsUnknownExceptionTo500") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    server.get("/boom",
               [](const Request&) -> Response
               { throw std::runtime_error("kaboom"); });

    check(server.listen(0));
    port = server.boundPort();
    performExchange(server, Request(baseUrl(port) + "/boom"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 500);
    check(ex.clientResponse.content == "kaboom");
};

auto tListenWithoutHandlerStartsServer =
    test("HttpServer/listenWithoutHandlerStartsServer") = []
{
    auto server = Server();
    auto port = 0;
    check(server.listen(0));
    port = server.boundPort();
    server.stop();
};

auto tAddRouteRegistersCustomMethod =
    test("HttpServer/addRouteRegistersArbitraryMethod") = []
{
    auto server = Server();
    auto port = 0;
    auto ex = Exchange();

    server.addRoute("PATCH",
                    "/r",
                    [&](const Request& req)
                    {
                        ex.received = req;
                        ex.handlerCalled = true;
                        auto res = Response();
                        res.statusCode = 200;
                        res.content = "patched";
                        return res;
                    });

    check(server.listen(0));
    port = server.boundPort();

    auto clientReq = Request(baseUrl(port) + "/r");
    clientReq.type = "PATCH";
    performExchange(server, clientReq, ex);

    check(ex.completed);
    check(ex.handlerCalled);
    check(ex.received.type == "PATCH");
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.content == "patched");
};
