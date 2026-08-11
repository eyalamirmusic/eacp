#pragma once

#include "../IPC/Messenger.h"
#include "../Rpc/AsyncCommand.h"

#include <unordered_map>

namespace eacp::IPC
{

// The dialing side of RpcServer. Calls issued before the dial lands wait in an
// outbox and flush on connection; calls still in flight when the conversation
// ends are rejected rather than left pending forever.
class RpcClient
{
public:
    explicit RpcClient(std::string_view name, Time::MS timeout = Time::MS {5000});

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;
    RpcClient(RpcClient&&) = delete;
    RpcClient& operator=(RpcClient&&) = delete;

    [[nodiscard]] bool isConnected() const { return messenger.isConnected(); }

    // Resolves with the server's JSON result on the main thread.
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

    // One handler per event name; assigning again replaces. Events whose
    // payload fails to deserialize are dropped.
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

    // Calls issued while the dial is in the air, flushed in order when it
    // lands.
    Vector<std::string> outbox;

    // Last member on purpose: its destructor stops the handlers above firing.
    Messenger messenger;
};

} // namespace eacp::IPC
