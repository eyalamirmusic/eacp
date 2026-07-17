#include "RpcClient.h"

namespace eacp::IPC
{

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
