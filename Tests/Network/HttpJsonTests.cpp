#include "Common.h"
#include <eacp/Network/HTTPServer/Json.h>

#include <vector>

using namespace nano;
using eacp::HTTP::Error;
using eacp::HTTP::Request;
using eacp::HTTP::RequestHandler;
using eacp::HTTP::Response;
using eacp::Threads::callAsync;
using eacp::Threads::stopEventLoop;
namespace Json = eacp::HTTP::Json;

namespace
{
std::atomic<int> nextPort {53001};

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
    Response clientResponse;
    bool completed = false;
};

void performExchange(eacp::HTTP::Server& server,
                     const Request& clientRequest,
                     Exchange& out)
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

struct CountResponse
{
    int n = 0;
    MIRO_REFLECT(n)
};

GreetResponse greet(const GreetRequest& req)
{
    if (req.name.empty())
        Json::throwError("name must not be empty");
    return {.greeting = "Hello, " + req.name + "!"};
}

CountResponse count(const Json::EmptyRequest&)
{
    return {.n = 7};
}
} // namespace

auto tJsonSetJsonSetsContentTypeAndBody =
    test("HttpJson/setJsonProducesJsonContent") = []
{
    auto res = Response();
    auto value = GreetResponse {.greeting = "hi"};
    Json::setJson(res, value);
    check(res.headers["Content-Type"] == "application/json");
    check(res.content.find("\"greeting\"") != std::string::npos);
    check(res.content.find("\"hi\"") != std::string::npos);
};

auto tJsonThrowErrorBuildsJsonResponse =
    test("HttpJson/throwErrorBuildsJsonEnvelope") = []
{
    auto caught = false;
    try
    {
        Json::throwError("not allowed", 403);
    }
    catch (const Error& e)
    {
        caught = true;
        check(e.statusCode == 403);
        check(e.response.statusCode == 403);
        check(e.response.headers.at("Content-Type") == "application/json");
        check(e.response.content.find("\"error\"") != std::string::npos);
        check(e.response.content.find("\"not allowed\"") != std::string::npos);
        check(e.response.content.find("\"status\"") != std::string::npos);
        check(e.response.content.find("403") != std::string::npos);
    }
    check(caught);
};

auto tJsonThrowErrorDefaultStatus = test("HttpJson/throwErrorDefaultsTo400") = []
{
    auto caught = false;
    try
    {
        Json::throwError("bad");
    }
    catch (const Error& e)
    {
        caught = true;
        check(e.statusCode == 400);
    }
    check(caught);
};

auto tJsonMakeHandlerDeserializesAndSerializes =
    test("HttpJson/makeHandlerDeserializesInputAndSerializesOutput") = []
{
    auto handler = Json::makeHandler(greet);
    auto req = Request();
    req.body = "{\"name\":\"World\"}";

    auto res = handler(req);
    check(res.statusCode == 200);
    check(res.headers["Content-Type"] == "application/json");
    check(res.content.find("\"greeting\"") != std::string::npos);
    check(res.content.find("Hello, World!") != std::string::npos);
};

auto tJsonMakeHandlerInvalidBodyThrows =
    test("HttpJson/makeHandlerThrowsOnInvalidBody") = []
{
    auto handler = Json::makeHandler(greet);
    auto req = Request();
    req.body = "not-json";

    auto caught = false;
    try
    {
        handler(req);
    }
    catch (const Error& e)
    {
        caught = true;
        check(e.statusCode == 400);
        check(e.response.headers.at("Content-Type") == "application/json");
    }
    check(caught);
};

auto tJsonMakeHandlerForwardsHandlerError =
    test("HttpJson/makeHandlerPropagatesHandlerError") = []
{
    auto handler = Json::makeHandler(greet);
    auto req = Request();
    req.body = "{\"name\":\"\"}";

    auto caught = false;
    try
    {
        handler(req);
    }
    catch (const Error& e)
    {
        caught = true;
        check(e.statusCode == 400);
    }
    check(caught);
};

auto tJsonServerTypedPostHappyPath = test("HttpJson/serverTypedPostHappyPath") = []
{
    auto server = Json::Server();
    auto port = 0;
    auto ex = Exchange();

    server.post("/greet", greet);
    check(server.listen(0));
    port = server.boundPort();

    performExchange(
        server, Request::post(baseUrl(port) + "/greet", "{\"name\":\"Eyal\"}"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.content.find("Hello, Eyal!") != std::string::npos);
    check(ex.clientResponse.headers["Content-Type"] == "application/json");
};

auto tJsonServerTypedPostInvalidJsonReturns400 =
    test("HttpJson/serverTypedPostInvalidJsonReturns400Envelope") = []
{
    auto server = Json::Server();
    auto port = 0;
    auto ex = Exchange();

    server.post("/greet", greet);
    check(server.listen(0));
    port = server.boundPort();

    performExchange(
        server, Request::post(baseUrl(port) + "/greet", "this-is-not-json"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 400);
    check(ex.clientResponse.content.find("\"error\"") != std::string::npos);
    check(ex.clientResponse.content.find("Invalid request body")
          != std::string::npos);
};

auto tJsonServerTypedPostHandlerThrownError =
    test("HttpJson/serverTypedPostForwardsHandlerThrowError") = []
{
    auto server = Json::Server();
    auto port = 0;
    auto ex = Exchange();

    server.post("/greet", greet);
    check(server.listen(0));
    port = server.boundPort();

    performExchange(
        server, Request::post(baseUrl(port) + "/greet", "{\"name\":\"\"}"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 400);
    check(ex.clientResponse.content.find("name must not be empty")
          != std::string::npos);
};

auto tJsonServerEmptyRequestHandler =
    test("HttpJson/serverTypedPostEmptyRequestAcceptsEmptyBody") = []
{
    auto server = Json::Server();
    auto port = 0;
    auto ex = Exchange();

    server.post("/count", count);
    check(server.listen(0));
    port = server.boundPort();

    performExchange(server, Request::post(baseUrl(port) + "/count", "{}"), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.content.find("\"n\"") != std::string::npos);
    check(ex.clientResponse.content.find("7") != std::string::npos);
};

auto tJsonServerInheritsRawPostOverload =
    test("HttpJson/serverStillAcceptsUntypedPost") = []
{
    auto server = Json::Server();
    auto port = 0;
    auto ex = Exchange();

    server.post("/raw",
                RequestHandler(
                    [](const Request&)
                    {
                        auto res = Response();
                        res.statusCode = 200;
                        res.setContent("plain", "text/plain");
                        return res;
                    }));
    check(server.listen(0));
    port = server.boundPort();

    performExchange(server, Request::post(baseUrl(port) + "/raw", ""), ex);

    check(ex.completed);
    check(ex.clientResponse.statusCode == 200);
    check(ex.clientResponse.content == "plain");
    check(ex.clientResponse.headers["Content-Type"] == "text/plain");
};
