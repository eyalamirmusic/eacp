#include "RpcClient.h"

#include <eacp/Network/Network.h>

namespace eacp::HTTP::Rpc
{

namespace
{

std::string extractError(const Miro::JSON& body)
{
    if (body.isObject())
    {
        auto& obj = body.asObject();
        auto it = obj.find("error");
        if (it != obj.end() && it->second.isString())
            return it->second.asString();
    }
    return {};
}

} // namespace

Client::Client(std::string baseUrlToUse)
    : baseUrl(std::move(baseUrlToUse))
{
}

Miro::JSON Client::invokeRaw(const std::string& command,
                             const Miro::JSON& payload) const
{
    auto envelope = Miro::JSON {Miro::Json::Object {}};
    envelope.asObject()["command"] = Miro::JSON {command};
    envelope.asObject()["payload"] = payload;

    auto req = Request::post(baseUrl, Miro::Json::print(envelope));
    req.headers["Content-Type"] = "application/json";

    auto res = req.perform();

    auto parsed = Miro::JSON {};
    try
    {
        parsed = Miro::Json::parse(res.content);
    }
    catch (const std::exception&)
    {
        if (res.statusCode < 200 || res.statusCode >= 300)
            throw Error(res.statusCode,
                        "RPC call '" + command + "' failed with status "
                            + std::to_string(res.statusCode));
        throw Error(500, "RPC call '" + command + "' returned non-JSON body");
    }

    if (res.statusCode < 200 || res.statusCode >= 300)
    {
        auto message = extractError(parsed);
        if (message.empty())
            message = "RPC call '" + command + "' failed with status "
                      + std::to_string(res.statusCode);
        throw Error(res.statusCode, message);
    }

    if (!parsed.isObject())
        throw Error(500, "RPC reply for '" + command + "' is not a JSON object");

    auto& obj = parsed.asObject();
    auto it = obj.find("result");
    if (it == obj.end())
        return Miro::JSON {};

    return it->second;
}

Invoke Client::asInvoker() const
{
    return [this](const std::string& command, const Miro::JSON& payload)
    { return invokeRaw(command, payload); };
}

} // namespace eacp::HTTP::Rpc
