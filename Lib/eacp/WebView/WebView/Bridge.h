#pragma once

#include "AsyncBridge.h"
#include "WebView.h"

#include <Miro/Miro.h>

#include <ea_data_structures/Pointers/Broadcaster.h>
#include <eacp/Core/Utils/Containers.h>

#include <string>
#include <unordered_map>

namespace eacp::Graphics
{

using EmptyMessage = Miro::EmptyValue;

// Transport adapter that routes WebView <-> C++ messages through a
// Miro::Bridge, which owns the command table and event registry; this class
// handles only the WebView wire format.
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
    WebViewBridge(WebView& webViewToUse);

    template <typename T>
    WebViewBridge(WebView& webViewToUse, T& api)
        : WebViewBridge(webViewToUse)
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

    WebView& webView;
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
