#pragma once

#include "../IPC/Messenger.h"
#include "../Rpc/AsyncCommand.h"

namespace eacp::IPC
{

// A Miro::Bridge mounted on the message channel. The wire shapes match the
// WebView bridge's - {id, command, payload} up, {reply, result | error} back,
// {event, payload} pushed. A main-thread object, like its Messenger.
class RpcServer
{
public:
    // Claims name (the ChannelServer rules apply). bridgeToUse must outlive
    // this server.
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

    // Last member on purpose: destroying it first retires the sessions, and
    // with them every handler capturing this.
    MessageServer server;
};

} // namespace eacp::IPC
