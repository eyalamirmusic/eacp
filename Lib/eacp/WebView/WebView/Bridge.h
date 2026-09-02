#pragma once

#include "AsyncBridge.h"
#include "ScriptHost.h"

#include <ea_data_structures/Pointers/Broadcaster.h>

#include <atomic>
#include <memory>

namespace eacp::Graphics
{

using EmptyMessage = Miro::EmptyValue;

// Transport adapter that routes page <-> C++ messages through a
// Miro::Bridge, which owns the command table and event registry; this class
// handles only the wire format.
//
// The page is reached through a ScriptHost, so the same bridge serves a
// Graphics::WebView and anything else that runs scripts against a document —
// the typed commands, the C++ -> page calls and the EACP_STATE broadcasts
// are the host's five members and nothing more.
//
// On construction it picks up every state declared via EACP_STATE in the
// linked TUs (see StateBridge.h) and broadcasts their changes on the wire
// automatically, unsubscribing when the bridge is destroyed.
//
// The Bridge can be shared with other transports (e.g. HTTP::Rpc::Server) so a
// single set of typed handlers — including those from MIRO_EXPORT_COMMAND — is
// served over multiple wires at once.
class WebViewBridge
{
public:
    WebViewBridge(ScriptHost& scriptHostToUse);

    template <typename T>
    WebViewBridge(ScriptHost& scriptHostToUse, T& api)
        : WebViewBridge(scriptHostToUse)
    {
        getBridge().use(api);
    }

    ~WebViewBridge();

    Miro::Bridge& getBridge() { return bridge; }

    // Controls how incoming commands are executed. Every command is async
    // on the TypeScript side regardless; this chooses where the bridge
    // runs the synchronous C++ handler. Defaults to MainThreadDeferred —
    // switch to WorkerThread only for handlers that are safe to run off
    // the main thread. See CommandExecution.
    void setCommandExecution(CommandExecution mode) { commandExecution = mode; }
    CommandExecution getCommandExecution() const { return commandExecution; }

    // Per-command override of the execution mode, keyed by command name; takes
    // precedence over the global default, so one slow command can go to a
    // worker thread without affecting the rest. Governs synchronous handlers
    // only — an async handler owns its own threading. Configure before commands
    // start flowing; consulted on the main thread.
    void setCommandExecution(const std::string& command, CommandExecution mode)
    {
        commandModes[command] = mode;
    }

    void clearCommandExecution(const std::string& command)
    {
        commandModes.erase(command);
    }

    // Calls a JavaScript function the page registered with
    // `window.eacp.expose(name, fn)` — the reverse of a command. The JS
    // function may be synchronous or `async`; either way its resolved
    // value comes back here as an Async that settles when the page
    // replies (or rejects if the function throws / is missing). Must be
    // called on the main thread.
    //
    // The typed overloads serialize the request and deserialize the
    // response through Miro, so the call site is just:
    //     bridge.call<Summary>("summarize", request)
    //         .then([](Summary s) { ... });
    Threads::Async<Miro::Json::Value> call(const std::string& functionName,
                                           const Miro::Json::Value& payload);

    Threads::Async<Miro::Json::Value> call(const std::string& functionName)
    {
        return call(functionName, Miro::Json::Value {});
    }

    template <typename Res, typename Req>
    Threads::Async<Res> call(const std::string& functionName, const Req& request)
    {
        return mapJson<Res>(call(functionName, Miro::toJSON(request)));
    }

    template <typename Res>
    Threads::Async<Res> call(const std::string& functionName)
    {
        return mapJson<Res>(call(functionName, Miro::Json::Value {}));
    }

private:
    void registerBuiltins();
    void onMessage(const std::string& body);
    void deliver(double id,
                 const Miro::Json::Value& result,
                 const std::string* error);
    bool handleCallReply(const Miro::Json::Value& message);
    void broadcast();

    // False once this bridge is gone. Every command hop reads it before
    // touching the bridge, because a command outlives the object it was
    // addressed to more easily than it looks: onMessage does not run the
    // handler, it QUEUES it (Rpc::runCommand, MainThreadDeferred) and queues
    // the reply behind that. A host destroyed in between — an app that
    // makes and unmakes windows on demand, rather than hiding them — leaves
    // those blocks pointing at a freed Bridge, and the page goes on posting
    // commands right up to the moment it is freed, so no amount of deferring
    // the destruction gets ahead of it.
    //
    // shared_ptr so the queued blocks can hold the flag itself rather than a
    // pointer into this object (the same guard HubAuthApi uses for its own
    // deferred fetch hops).
    std::shared_ptr<std::atomic<bool>> alive =
        std::make_shared<std::atomic<bool>>(true);

    ScriptHost& scriptHost;
    Miro::Bridge bridge;
    EA::Listener emitListener;
    Vector<OwningPointer<EA::Listener>> stateListeners;
    CommandExecution commandExecution = CommandExecution::MainThreadDeferred;
    std::unordered_map<std::string, CommandExecution> commandModes;

    // Outstanding C++ -> page calls, keyed by the id sent to
    // window.__eacp.callFunction and echoed back in the reply envelope.
    double callCounter = 0;
    std::unordered_map<double, Threads::AsyncPromise<Miro::Json::Value>>
        pendingCalls;
};

} // namespace eacp::Graphics
