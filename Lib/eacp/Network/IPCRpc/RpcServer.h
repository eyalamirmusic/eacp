#pragma once

#include "../IPC/Messenger.h"
#include "../Rpc/AsyncCommand.h"

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

} // namespace eacp::IPC
