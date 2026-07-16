#pragma once

#include "../Rpc/AsyncCommand.h"
#include "Messenger.h"

#include <unordered_map>

namespace eacp::IPC
{

// Typed RPC between this user's processes: a Miro::Bridge mounted on the
// message channel. The server side turns each incoming envelope into a
// bridge dispatch and answers by id; every bridge emit fans out to all
// connected clients as an event. The wire shapes are the WebView bridge's
// - {id, command, payload} up, {reply, result | error} back, {event,
// payload} pushed - so one Bridge can serve a window and a sibling
// process with the same handlers.
//
// Main-thread objects, like the Messenger they ride on.
class RpcServer
{
public:
    // Claims name (the ChannelServer rules apply) and serves bridgeToUse's
    // commands to every client that dials in. The bridge must outlive
    // this server, the same contract every Miro transport has.
    RpcServer(std::string_view name, Miro::Bridge& bridgeToUse);

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;
    RpcServer(RpcServer&&) = delete;
    RpcServer& operator=(RpcServer&&) = delete;

    // Where incoming command handlers run; see Rpc::CommandExecution.
    void setCommandExecution(Rpc::CommandExecution mode) { commandExecution = mode; }

    [[nodiscard]] int connectedClients() const { return clients.size(); }

    Callback onClientConnected = [] {};
    Callback onClientDisconnected = [] {};

private:
    void serve(Messenger& client);
    void handle(Messenger& client, const std::string& body);
    void broadcast();

    Miro::Bridge& bridge;
    Rpc::CommandExecution commandExecution =
        Rpc::CommandExecution::MainThreadDeferred;
    Vector<Messenger*> clients;
    EA::Listener emitListener;

    // Last member on purpose: destroying the MessageServer first is what
    // retires the sessions - and with them every handler capturing this.
    MessageServer server;
};

// The dialing side. Calls are safe to issue immediately after
// construction: anything sent before the dial lands waits in an outbox
// and flushes on connection, and every call still in flight when the
// conversation ends is rejected rather than left pending forever.
class RpcClient
{
public:
    explicit RpcClient(std::string_view name, Time::MS timeout = Time::MS {5000});

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;
    RpcClient(RpcClient&&) = delete;
    RpcClient& operator=(RpcClient&&) = delete;

    [[nodiscard]] bool isConnected() const { return messenger.isConnected(); }

    // Invokes a command on the server, resolving with its JSON result on
    // the main thread. The typed overloads serialize the request and
    // deserialize the response through Miro, so a call site is just:
    //     rpc.call<DotTotal>("addDot", dot).then([](DotTotal t) { ... });
    Threads::Async<Miro::Json::Value> call(const std::string& command,
                                           const Miro::Json::Value& payload);

    Threads::Async<Miro::Json::Value> call(const std::string& command)
    {
        return call(command, Miro::Json::Value {});
    }

    template <typename Res, typename Req>
    Threads::Async<Res> call(const std::string& command, const Req& request)
    {
        return Rpc::mapJson<Res>(call(command, Miro::toJSON(request)));
    }

    template <typename Res>
    Threads::Async<Res> call(const std::string& command)
    {
        return Rpc::mapJson<Res>(call(command, Miro::Json::Value {}));
    }

    // Subscribes to a server-pushed event (a Bridge::emit on the other
    // side), one handler per event name - assigning again replaces. The
    // typed form deserializes the payload and drops events that fail to;
    // the Callback form ignores the payload entirely.
    template <typename T>
    void on(const std::string& event, std::function<void(const T&)> handler)
    {
        events[event] =
            [handler = std::move(handler)](const Miro::Json::Value& payload)
        {
            try
            {
                handler(Miro::createFromJSON<T>(payload));
            }
            catch (const std::exception&)
            {
            }
        };
    }

    void on(const std::string& event, const Callback& handler)
    {
        events[event] = [handler](const Miro::Json::Value&) { handler(); };
    }

    Callback onConnected = [] {};
    Callback onDisconnected = [] {};

private:
    void handle(const std::string& body);
    void settle(double id, const Miro::Json::Object& message);
    void rejectPending(const std::string& reason);

    double callCounter = 0;
    std::unordered_map<double, Threads::AsyncPromise<Miro::Json::Value>>
        pendingCalls;
    std::unordered_map<std::string, std::function<void(const Miro::Json::Value&)>>
        events;

    // Calls issued while the dial is still in the air, flushed in order
    // the moment it lands.
    Vector<std::string> outbox;

    // Last member on purpose: the messenger's destructor is what
    // guarantees none of the handlers above fire again.
    Messenger messenger;
};

} // namespace eacp::IPC
