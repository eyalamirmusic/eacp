#include "Rpc.h"

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

    if (idIt == object.end() || commandIt == object.end()
        || !commandIt->second.isString())
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

void RpcServer::serve(Messenger& client)
{
    clients.add(&client);
    onClientConnected();

    client.onDisconnected = [this, leaving = &client]
    {
        clients.eraseIf([leaving](Messenger* candidate)
                        { return candidate == leaving; });
        onClientDisconnected();
    };

    client.onMessage = [this, from = &client](const std::string& body)
    { handle(*from, body); };
}

void RpcServer::handle(Messenger& client, const std::string& body)
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

    // The reply lands back on the main thread, where the client may
    // already have hung up - a departed session is simply not written to.
    Rpc::resolveWith(std::move(work),
                     [this, id = envelope->id, target = &client](
                         const Miro::Json::Value& result, const std::string* error)
                     {
                         if (clients.contains(target))
                             target->send(printReply(id, result, error));
                     });
}

void RpcServer::broadcast()
{
    auto body = Miro::JSON {Miro::Json::Object {}};
    body.asObject()["event"] = Miro::JSON {std::string {bridge.currentEvent()}};
    body.asObject()["payload"] = bridge.currentPayload();

    auto text = Miro::Json::print(body);

    for (auto* client: clients)
        client->send(text);
}

RpcClient::RpcClient(std::string_view name, Time::MS timeout)
    : messenger(name, timeout)
{
    messenger.onConnected = [this]
    {
        auto queued = std::move(outbox);

        for (auto& text: queued)
            messenger.send(text);

        onConnected();
    };

    messenger.onDisconnected = [this]
    {
        outbox.clear();
        rejectPending("the RPC channel disconnected");
        onDisconnected();
    };

    messenger.onMessage = [this](const std::string& body) { handle(body); };
}

Threads::Async<Miro::Json::Value> RpcClient::call(const std::string& command,
                                                  const Miro::Json::Value& payload)
{
    auto id = ++callCounter;
    auto promise = Threads::AsyncPromise<Miro::Json::Value> {};
    pendingCalls.emplace(id, promise);

    auto body = Miro::JSON {Miro::Json::Object {}};
    body.asObject()["id"] = Miro::JSON {id};
    body.asObject()["command"] = Miro::JSON {command};
    body.asObject()["payload"] = payload;

    auto text = Miro::Json::print(body);

    if (messenger.isConnected())
        messenger.send(text);
    else
        outbox.add(std::move(text));

    return promise.get();
}

void RpcClient::handle(const std::string& body)
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

    if (!value.isObject())
        return;

    auto& object = value.asObject();

    if (auto replyIt = object.find("reply");
        replyIt != object.end() && replyIt->second.isNumber())
    {
        settle(replyIt->second.asNumber(), object);
        return;
    }

    auto eventIt = object.find("event");

    if (eventIt == object.end() || !eventIt->second.isString())
        return;

    auto handlerIt = events.find(eventIt->second.asString());

    if (handlerIt == events.end())
        return;

    auto payloadIt = object.find("payload");
    handlerIt->second(payloadIt != object.end() ? payloadIt->second
                                                : Miro::Json::Value {});
}

void RpcClient::settle(double id, const Miro::Json::Object& message)
{
    auto it = pendingCalls.find(id);

    if (it == pendingCalls.end())
        return;

    auto promise = it->second;
    pendingCalls.erase(it);

    if (auto errorIt = message.find("error");
        errorIt != message.end() && errorIt->second.isString())
    {
        promise.reject(errorIt->second.asString());
        return;
    }

    auto resultIt = message.find("result");
    promise.resolve(resultIt != message.end() ? resultIt->second
                                              : Miro::Json::Value {});
}

void RpcClient::rejectPending(const std::string& reason)
{
    auto failed = std::move(pendingCalls);
    pendingCalls.clear();

    for (auto& [id, promise]: failed)
        promise.reject(reason);
}

} // namespace eacp::IPC
