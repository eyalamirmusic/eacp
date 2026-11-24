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
GreetResponse rpcGreet(const GreetRequest& req)
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
    bridge.on<GreetRequest, GreetResponse>("greet", &rpcGreet);

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
    bridge.on<GreetRequest, GreetResponse>("greet", &rpcGreet);

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
    bridge.on<GreetRequest, GreetResponse>("greet", &rpcGreet);
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
    bridge.on<GreetRequest, GreetResponse>("greet", &rpcGreet);
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
    bridge.on<GreetRequest, GreetResponse>("greet", &rpcGreet);
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

// ---------- Client timeouts ----------

namespace
{
struct CaughtError
{
    bool thrown = false;
    int statusCode = -1;
    std::string message;
};

CaughtError greetCatchingError(const Rpc::Client& client)
{
    auto caught = CaughtError();

    try
    {
        auto payload = Miro::toJSON(GreetRequest {.name = "x"});
        (void) client.invokeRaw("greet", payload);
    }
    catch (const Error& e)
    {
        caught.thrown = true;
        caught.statusCode = e.statusCode;
        caught.message = e.what();
    }

    return caught;
}

bool mentionsATimeout(const std::string& message)
{
    return eacp::Strings::toLower(message).find("timed out") != std::string::npos;
}

Rpc::ClientOptions timeoutOf(eacp::Time::MS limit)
{
    auto options = Rpc::ClientOptions();
    options.timeout = limit;
    return options;
}
} // namespace

auto tRpcClientOptionsDefaultToUnset =
    test("HttpRpc/clientOptionsDefaultToUnset") = []
{
    auto options = Rpc::ClientOptions();
    check(options.timeout.count == 0);
    check(options.connectTimeout.count == 0);
    check(options.idleTimeout.count == 0);
};

auto tRpcClientTimeoutEndsStalledCall =
    test("HttpRpc/clientTimeoutEndsACallTheServerIsStillWorkingOn") = []
{
    auto httpServer = eacp::HTTP::Server(threadPoolOptions());
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};
    auto stalled = StallGate();

    auto stallThenGreet = [&](const GreetRequest& req)
    {
        stalled.wait();
        return rpcGreet(req);
    };

    bridge.on<GreetRequest, GreetResponse>("greet", stallThenGreet);
    check(httpServer.listen(0));

    auto port = httpServer.boundPort();
    auto caught = CaughtError();
    auto gaveUpEarly = false;

    runWithServer(port,
                  httpServer,
                  [&]
                  {
                      auto client = Rpc::Client {baseUrl(port) + "/rpc",
                                                 timeoutOf(eacp::Time::MS {200})};

                      auto beforeTheServerReplies =
                          eacp::Time::Deadline {withoutWaitingForTheServer};
                      caught = greetCatchingError(client);
                      gaveUpEarly = !beforeTheServerReplies.expired();
                      stalled.release();
                  });

    check(caught.thrown);
    check(gaveUpEarly);
    check(caught.statusCode == 0);
    check(mentionsATimeout(caught.message));
    check(caught.message.find("'greet'") != std::string::npos);
};

auto tRpcClientGenerousTimeoutCompletes =
    test("HttpRpc/clientTimeoutLongerThanTheCallStillCompletes") = []
{
    auto httpServer = eacp::HTTP::Server(threadPoolOptions());
    auto bridge = Miro::Bridge {};
    auto rpc = Rpc::Server {httpServer, bridge};

    auto slowGreet = [](const GreetRequest& req)
    {
        eacp::Time::sleep(eacp::Time::MS {300});
        return rpcGreet(req);
    };

    bridge.on<GreetRequest, GreetResponse>("greet", slowGreet);
    check(httpServer.listen(0));

    auto port = httpServer.boundPort();
    auto reply = GreetResponse {};

    runWithServer(port,
                  httpServer,
                  [&]
                  {
                      auto client = Rpc::Client {baseUrl(port) + "/rpc",
                                                 timeoutOf(eacp::Time::MS {4000})};
                      reply = client.invoke<GreetResponse>(
                          "greet", GreetRequest {.name = "Patient"});
                  });

    check(reply.greeting == "Hello, Patient!");
};

auto tRpcClientReportsTransportError =
    test("HttpRpc/clientReportsTheTransportErrorWhenThereIsNoReply") = []
{
    auto caught = CaughtError();

    auto callRefusedPort = [&]
    {
        auto client = Rpc::Client {"http://127.0.0.1:1/rpc"};
        caught = greetCatchingError(client);
    };

    check(runOffTheLoop(callRefusedPort));

    check(caught.thrown);
    check(caught.statusCode == 0);

    auto prefix = std::string("RPC call 'greet' failed: ");
    check(caught.message.starts_with(prefix));
    check(caught.message.size() > prefix.size());
};
