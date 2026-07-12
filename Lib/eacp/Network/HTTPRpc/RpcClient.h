#pragma once

#include <eacp/Network/Network.h>

#include <Miro/Reflect.h>

namespace eacp::HTTP::Rpc
{

using Invoke =
    std::function<Miro::JSON(const std::string& command, const Miro::JSON& payload)>;

// Typed client for the wire protocol exposed by Rpc::Server. POSTs
// { "command", "payload" } envelopes to `baseUrl` and parses the
// reply, throwing HTTP::Error on non-2xx with the server-returned
// error string.
//
// `asInvoker()` returns an Invoke callable suitable for the Client
// class emitted by Miro::Cpp::formatClientHeader, so a generated
// typed C++ client can drive any RPC server through this transport.
class Client
{
public:
    explicit Client(std::string baseUrlToUse);

    template <typename Res, typename Req>
    Res invoke(const std::string& command, const Req& req)
    {
        auto result = invokeRaw(command, Miro::toJSON(req));
        auto out = Res {};
        Miro::fromJSON(out, result);
        return out;
    }

    template <typename Res>
    Res invoke(const std::string& command)
    {
        auto result = invokeRaw(command, Miro::Json::Object());
        auto out = Res {};
        Miro::fromJSON(out, result);
        return out;
    }

    Miro::JSON invokeRaw(const std::string& command,
                         const Miro::JSON& payload) const;

    Invoke asInvoker() const;

private:
    std::string baseUrl;
};

} // namespace eacp::HTTP::Rpc
