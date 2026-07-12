#include "RpcServer.h"

#include <Miro/Json.h>

namespace eacp::HTTP::Rpc
{

namespace
{

Response makeJsonResponse(int status, const Miro::JSON& body)
{
    auto res = Response();
    res.statusCode = status;
    res.setContent(Miro::Json::print(body), "application/json");
    return res;
}

Response makeErrorResponse(int status, const std::string& message)
{
    auto body = Miro::JSON {Miro::Json::Object {}};
    body.asObject()["error"] = Miro::JSON {message};
    return makeJsonResponse(status, body);
}

Response makeResultResponse(const Miro::JSON& result)
{
    auto body = Miro::JSON {Miro::Json::Object {}};
    body.asObject()["result"] = result;
    return makeJsonResponse(200, body);
}

} // namespace

Server::Server(eacp::HTTP::Server& server,
               Miro::Bridge& bridgeToUse,
               std::string basePathToUse)
    : bridge(bridgeToUse)
    , basePath(std::move(basePathToUse))
{
    server.post(basePath, [this](const Request& req) { return handle(req); });
}

Response Server::handle(const Request& req)
{
    auto body = Miro::JSON {};
    try
    {
        body = Miro::Json::parse(req.body);
    }
    catch (const std::exception& e)
    {
        return makeErrorResponse(400,
                                 std::string {"Invalid JSON body: "} + e.what());
    }

    if (!body.isObject())
        return makeErrorResponse(400,
                                 "Request body must be a JSON object with "
                                 "'command' and 'payload' fields");

    auto& obj = body.asObject();
    auto cmdIt = obj.find("command");
    if (cmdIt == obj.end() || !cmdIt->second.isString())
        return makeErrorResponse(400, "Missing or non-string 'command' field");

    auto command = cmdIt->second.asString();

    auto payloadIt = obj.find("payload");
    auto payload = payloadIt != obj.end() ? payloadIt->second
                                          : Miro::JSON {Miro::Json::Object {}};

    try
    {
        auto result = bridge.dispatch(command, payload);
        return makeResultResponse(result);
    }
    catch (const Miro::UnknownCommandError& e)
    {
        return makeErrorResponse(404, e.what());
    }
    catch (const Error& e)
    {
        return makeErrorResponse(e.statusCode, e.what());
    }
    catch (const std::exception& e)
    {
        return makeErrorResponse(500, e.what());
    }
}

} // namespace eacp::HTTP::Rpc
