#pragma once

#include "AsyncBridge.h"
#include "WebView.h"

#include <ea_data_structures/Pointers/Broadcaster.h>

namespace eacp::Graphics
{

using EmptyMessage = Miro::EmptyValue;

// Puts the WebView wire format over a Miro::Bridge, which owns the command
// table and event registry and can be shared with other transports. Subscribes
// to every EACP_STATE in the linked TUs for its lifetime.
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

    // Where synchronous C++ handlers run; commands stay async in TypeScript
    // either way. Use WorkerThread only for main-thread-independent handlers.
    void setCommandExecution(CommandExecution mode) { commandExecution = mode; }
    CommandExecution getCommandExecution() const { return commandExecution; }

    // Overrides the global default for one command. Configure before commands
    // start flowing; async handlers own their own threading regardless.
    void setCommandExecution(const std::string& command, CommandExecution mode)
    {
        commandModes[command] = mode;
    }

    void clearCommandExecution(const std::string& command)
    {
        commandModes.erase(command);
    }

    // Calls a JS function the page registered with `window.eacp.expose()`.
    // Rejects if that function throws or is missing. Main thread only.
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

    // Outstanding C++ -> page calls, keyed by the id echoed in the reply.
    double callCounter = 0;
    std::unordered_map<double, Threads::AsyncPromise<Miro::Json::Value>>
        pendingCalls;
};

} // namespace eacp::Graphics
