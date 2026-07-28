#include "Common.h"
#include <eacp/Network/HTTPRpc/RpcClient.h>
#include <eacp/Network/HTTPRpc/RpcServer.h>

#include <thread>

using namespace nano;
using eacp::HTTP::Error;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::Threads::callAsync;
using eacp::Threads::stopEventLoop;
namespace Rpc = eacp::HTTP::Rpc;

namespace
{

// Every server here binds port 0 and reports back what it got. Fixed ports do
// not survive this suite: NanoTest's CTest discovery runs each case in its own
// process, so a per-process counter hands every concurrent case the same number,
// and SO_REUSEADDR lets them all bind it and steal each other's connections.
std::string baseUrl(int port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

struct Exchange
{
    Response clientResponse;
    bool completed = false;
};

void performExchange(eacp::HTTP::Server& server,
                     const Request& clientRequest,
                     Exchange& out)
{
    auto worker = std::thread();

    auto stopped = eacp::Threads::runEventLoopFor(
        eacp::Time::MS {5000},
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

struct GreetRequest
{
    std::string name;
    MIRO_REFLECT(name)
};

struct GreetResponse
{
    std::string greeting;
    MIRO_REFLECT(greeting)
};

GreetResponse greet(const GreetRequest& req)
{
    if (req.name.empty())
        eacp::HTTP::throwError("name must not be empty");
    return {.greeting = "Hello, " + req.name + "!"};
}

GreetResponse boom(const GreetRequest&)
{
    throw std::runtime_error("kaboom");
}

} // namespace

auto tRpcDispatchesTypedHandler =
    test("HttpRpc/serverDispatchesTypedHandlerAndReturnsResult") = []
{
    auto httpServer = eacp::HTTP::Server();
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};
    bridge.on<GreetRequest, GreetResponse>("greet", &greet);

    auto ok = httpServer.listen(0);
    check(ok);

    auto port = httpServer.boundPort();

    auto exchange = Exchange();
    auto clientReq = Request::post(
        baseUrl(port) + "/rpc", R"({"command":"greet","payload":{"name":"World"}})");
    clientReq.headers["Content-Type"] = "application/json";

    performExchange(httpServer, clientReq, exchange);

    check(exchange.completed);
    check(exchange.clientResponse.statusCode == 200);
    check(exchange.clientResponse.content.find("\"result\"") != std::string::npos);
    check(exchange.clientResponse.content.find("Hello, World!")
          != std::string::npos);
};

auto tRpcUnknownCommandReturns404 =
    test("HttpRpc/serverReturns404OnUnknownCommand") = []
{
    auto httpServer = eacp::HTTP::Server();
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};

    auto ok = httpServer.listen(0);
    check(ok);

    auto port = httpServer.boundPort();

    auto exchange = Exchange();
    auto clientReq = Request::post(baseUrl(port) + "/rpc",
                                   R"({"command":"missing","payload":{}})");
    clientReq.headers["Content-Type"] = "application/json";

    performExchange(httpServer, clientReq, exchange);

    check(exchange.completed);
    check(exchange.clientResponse.statusCode == 404);
    check(exchange.clientResponse.content.find("\"error\"") != std::string::npos);
};

auto tRpcHttpErrorPropagatesStatus =
    test("HttpRpc/serverPropagatesHttpErrorStatus") = []
{
    auto httpServer = eacp::HTTP::Server();
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};
    bridge.on<GreetRequest, GreetResponse>("greet", &greet);

    auto ok = httpServer.listen(0);
    check(ok);

    auto port = httpServer.boundPort();

    auto exchange = Exchange();
    auto clientReq = Request::post(baseUrl(port) + "/rpc",
                                   R"({"command":"greet","payload":{"name":""}})");
    clientReq.headers["Content-Type"] = "application/json";

    performExchange(httpServer, clientReq, exchange);

    check(exchange.completed);
    check(exchange.clientResponse.statusCode == 400);
    check(exchange.clientResponse.content.find("\"error\"") != std::string::npos);
    check(exchange.clientResponse.content.find("name must not be empty")
          != std::string::npos);
};

auto tRpcUnexpectedExceptionReturns500 =
    test("HttpRpc/serverReturns500OnUnexpectedException") = []
{
    auto httpServer = eacp::HTTP::Server();
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};
    bridge.on<GreetRequest, GreetResponse>("boom", &boom);

    auto ok = httpServer.listen(0);
    check(ok);

    auto port = httpServer.boundPort();

    auto exchange = Exchange();
    auto clientReq = Request::post(baseUrl(port) + "/rpc",
                                   R"({"command":"boom","payload":{"name":"x"}})");
    clientReq.headers["Content-Type"] = "application/json";

    performExchange(httpServer, clientReq, exchange);

    check(exchange.completed);
    check(exchange.clientResponse.statusCode == 500);
    check(exchange.clientResponse.content.find("kaboom") != std::string::npos);
};

auto tRpcMalformedBodyReturns400 =
    test("HttpRpc/serverReturns400OnMalformedJson") = []
{
    auto httpServer = eacp::HTTP::Server();
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};

    auto ok = httpServer.listen(0);
    check(ok);

    auto port = httpServer.boundPort();

    auto exchange = Exchange();
    auto clientReq = Request::post(baseUrl(port) + "/rpc", "not-json");
    clientReq.headers["Content-Type"] = "application/json";

    performExchange(httpServer, clientReq, exchange);

    check(exchange.completed);
    check(exchange.clientResponse.statusCode == 400);
    check(exchange.clientResponse.content.find("Invalid JSON body")
          != std::string::npos);
};

// ---------- Client side ----------

namespace
{

template <typename Fn>
void runWithServer(int port, eacp::HTTP::Server& server, Fn&& clientWork)
{
    auto worker = std::thread();
    auto stopped = eacp::Threads::runEventLoopFor(
        eacp::Time::MS {5000},
        [&]
        {
            worker = std::thread(
                [&]
                {
                    clientWork();
                    callAsync([] { stopEventLoop(); });
                });
        });
    worker.join();
    server.stop();
    check(stopped);
}

} // namespace

auto tRpcClientRoundTripsTypedCall = test("HttpRpc/clientRoundTripsTypedCall") = []
{
    auto httpServer = eacp::HTTP::Server();
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};
    bridge.on<GreetRequest, GreetResponse>("greet", &greet);
    auto ok = httpServer.listen(0);
    check(ok);

    auto port = httpServer.boundPort();

    auto reply = GreetResponse {};

    runWithServer(port,
                  httpServer,
                  [&]
                  {
                      auto client = Rpc::Client {baseUrl(port) + "/rpc"};
                      reply = client.invoke<GreetResponse>(
                          "greet", GreetRequest {.name = "Eyal"});
                  });

    check(reply.greeting == "Hello, Eyal!");
};

auto tRpcClientThrowsOnHandlerError = test("HttpRpc/clientThrowsOnHandlerError") = []
{
    auto httpServer = eacp::HTTP::Server();
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};
    bridge.on<GreetRequest, GreetResponse>("greet", &greet);
    auto ok = httpServer.listen(0);
    check(ok);

    auto port = httpServer.boundPort();

    auto caughtStatus = 0;
    auto caughtMessage = std::string {};

    runWithServer(port,
                  httpServer,
                  [&]
                  {
                      try
                      {
                          auto client = Rpc::Client {baseUrl(port) + "/rpc"};
                          (void) client.invoke<GreetResponse>(
                              "greet", GreetRequest {.name = ""});
                      }
                      catch (const Error& e)
                      {
                          caughtStatus = e.statusCode;
                          caughtMessage = e.what();
                      }
                  });

    check(caughtStatus == 400);
    check(caughtMessage.find("name must not be empty") != std::string::npos);
};

auto tRpcClientThrowsOnUnknownCommand =
    test("HttpRpc/clientThrowsOnUnknownCommand") = []
{
    auto httpServer = eacp::HTTP::Server();
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};
    auto ok = httpServer.listen(0);
    check(ok);

    auto port = httpServer.boundPort();

    auto caughtStatus = 0;

    runWithServer(port,
                  httpServer,
                  [&]
                  {
                      try
                      {
                          auto client = Rpc::Client {baseUrl(port) + "/rpc"};
                          (void) client.invoke<GreetResponse>(
                              "missing", GreetRequest {.name = "x"});
                      }
                      catch (const Error& e)
                      {
                          caughtStatus = e.statusCode;
                      }
                  });

    check(caughtStatus == 404);
};

auto tRpcAsInvokerWorks = test("HttpRpc/asInvokerProducesUsableInvokeCallable") = []
{
    auto httpServer = eacp::HTTP::Server();
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};
    bridge.on<GreetRequest, GreetResponse>("greet", &greet);
    auto ok = httpServer.listen(0);
    check(ok);

    auto port = httpServer.boundPort();

    auto out = std::string {};

    runWithServer(port,
                  httpServer,
                  [&]
                  {
                      auto client = Rpc::Client {baseUrl(port) + "/rpc"};
                      auto invoker = client.asInvoker();

                      auto payload = Miro::JSON {Miro::Json::Object {}};
                      payload.asObject()["name"] =
                          Miro::JSON {std::string {"Invoker"}};

                      auto result = invoker("greet", payload);
                      check(result.isObject());
                      out = result["greeting"].asString();
                  });

    check(out == "Hello, Invoker!");
};
