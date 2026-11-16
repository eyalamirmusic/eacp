#include "RpcServer.h"

namespace eacp::IPC
{
namespace
{
struct Envelope
{
    double id = 0;
    std::string command;
    Miro::Json::Value payload;
};

std::optional<Envelope> parseEnvelope(const Miro::Json::Value& value)
{
    if (!value.isObject())
        return std::nullopt;

    auto& object = value.asObject();

    auto idIt = object.find("id");
    auto commandIt = object.find("command");
    auto payloadIt = object.find("payload");

    if (idIt == object.end())
        return std::nullopt;

    if (commandIt == object.end() || !commandIt->second.isString())
        return std::nullopt;

    auto envelope = Envelope {};
    envelope.id = idIt->second.isNumber() ? idIt->second.asNumber() : 0.0;
    envelope.command = commandIt->second.asString();
    envelope.payload =
        payloadIt != object.end() ? payloadIt->second : Miro::Json::Value {};
    return envelope;
}

std::string
    printReply(double id, const Miro::Json::Value& result, const std::string* error)
{
    auto body = Miro::JSON {Miro::Json::Object {}};
    body.asObject()["reply"] = Miro::JSON {id};

    if (error != nullptr)
        body.asObject()["error"] = Miro::JSON {*error};
    else
        body.asObject()["result"] = result;

    return Miro::Json::print(body);
}
} // namespace

RpcServer::RpcServer(std::string_view name, Miro::Bridge& bridgeToUse)
    : bridge(bridgeToUse)
    , emitListener(
          bridge.onEmit,
          [this] { broadcast(); },
          EA::Listener::Modes::TriggerOnEvent)
    , server(name)
{
    server.onClient = [this](Messenger& client) { serve(client); };
}

void RpcServer::setCommandExecution(Rpc::CommandExecution mode)
{
    commandExecution = mode;
}

int RpcServer::connectedClients() const
{
    return clients.size();
}

void RpcServer::serve(Messenger& client)
{
    auto sessionId = SessionId {++nextSessionId};
    clients.add({&client, sessionId});
    onClientConnected();

    client.onDisconnected = [this, sessionId]
    {
        clients.eraseIf([sessionId](const Session& candidate)
                        { return candidate.id == sessionId; });
        onClientDisconnected();
    };

    client.onMessage = [this, sessionId](const std::string& body)
    { handle(sessionId, body); };
}

void RpcServer::reply(SessionId sessionId, const std::string& text)
{
    for (auto& session: clients)
    {
        if (session.id == sessionId)
            session.messenger->send(text);
    }
}

void RpcServer::handle(SessionId sessionId, const std::string& body)
{
    auto value = Miro::Json::Value {};

    try
    {
        value = Miro::Json::parse(body);
    }
    catch (const std::exception&)
    {
        return;
    }

    auto envelope = parseEnvelope(value);

    if (!envelope)
        return;

    auto invoke = [this, command = envelope->command, payload = envelope->payload](
                      Miro::Resolve resolve)
    { bridge.dispatchAsync(command, payload, resolve); };

    auto work = Rpc::runCommand(commandExecution, std::move(invoke));

    // The reply lands back on the main thread, where the client may already
    // have hung up. Sessions are found by an id never reused, not by address:
    // a newer session can be allocated where a departed one lived.
    Rpc::resolveWith(std::move(work),
                     [this, id = envelope->id, sessionId](
                         const Miro::Json::Value& result, const std::string* error)
                     { reply(sessionId, printReply(id, result, error)); });
}

void RpcServer::broadcast()
{
    auto body = Miro::JSON {Miro::Json::Object {}};
    body.asObject()["event"] = Miro::JSON {std::string {bridge.currentEvent()}};
    body.asObject()["payload"] = bridge.currentPayload();

    auto text = Miro::Json::print(body);

    for (auto& session: clients)
        session.messenger->send(text);
}

OwningPointer<RpcServer> createRPCServer(std::string_view name,
                                         Miro::Bridge& bridgeToUse)
{
    try
    {
        return new RpcServer(name, bridgeToUse);
    }
    catch (std::exception& e)
    {
        LOG("Can't create server ", e.what());
    }

    return nullptr;
}

} // namespace eacp::IPC
