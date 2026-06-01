#include "Bridge.h"

#include "StateBridge.h"

#include <optional>

namespace eacp::Graphics
{
namespace
{
constexpr const char* bridgeChannel = "__eacpBridge";

constexpr const char* bridgeShim = R"JS(
(function() {
    if (window.eacp) return;

    var pending = new Map();
    var counter = 0;
    var listeners = new Map();
    var exposed = new Map();

    function post(envelope) {
        var raw = JSON.stringify(envelope);
        if (window.webkit && window.webkit.messageHandlers
            && window.webkit.messageHandlers.__eacpBridge) {
            window.webkit.messageHandlers.__eacpBridge.postMessage(raw);
        } else if (window.__eacpBridge && window.__eacpBridge.postMessage) {
            window.__eacpBridge.postMessage(raw);
        } else {
            console.error('eacp bridge: no transport available');
        }
    }

    window.eacp = {
        invoke: function(command, payload) {
            return new Promise(function(resolve, reject) {
                var id = ++counter;
                pending.set(id, { resolve: resolve, reject: reject });
                post({
                    id: id,
                    command: command,
                    payload: payload === undefined ? null : payload
                });
            });
        },
        on: function(event, handler) {
            var arr = listeners.get(event) || [];
            arr.push(handler);
            listeners.set(event, arr);
            return function() {
                var current = listeners.get(event) || [];
                listeners.set(event,
                    current.filter(function(h) { return h !== handler; }));
            };
        },
        // Registers a function the native side can call via
        // WebViewBridge::call(name, ...). The function may be sync or
        // async (return a value or a Promise); either way its resolved
        // result is sent back to C++.
        expose: function(name, fn) { exposed.set(name, fn); return fn; }
    };

    window.__eacp = {
        deliver: function(id, result, error) {
            var entry = pending.get(id);
            if (!entry) return;
            pending.delete(id);
            if (error) entry.reject(new Error(error));
            else entry.resolve(result);
        },
        dispatch: function(event, payload) {
            var arr = listeners.get(event) || [];
            for (var i = 0; i < arr.length; i++) {
                try { arr[i](payload); }
                catch (e) { console.error('eacp event handler error', e); }
            }
        },
        // Native -> page call. Looks up an exposed function, awaits its
        // result (Promise or plain value), and posts a reply envelope
        // keyed by `id` back to C++ — mirroring how invoke() replies are
        // delivered the other way.
        callFunction: function(id, name, payload) {
            Promise.resolve().then(function() {
                var fn = exposed.get(name);
                if (typeof fn !== 'function')
                    throw new Error("eacp: no exposed function '" + name + "'");
                return fn(payload);
            }).then(function(result) {
                post({ reply: id, result: result === undefined ? null : result });
            }, function(err) {
                post({ reply: id,
                       error: String(err && err.message ? err.message : err) });
            });
        }
    };
})();
)JS";

std::string jsStringLiteral(std::string_view value)
{
    auto out = std::string {"\""};
    out.reserve(value.size() + 2);

    for (auto c: value)
    {
        switch (c)
        {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += c;
                }
                break;
        }
    }

    out += '"';
    return out;
}
} // namespace

WebViewBridge::WebViewBridge(WebView& webViewToUse)
    : webView(webViewToUse)
    , emitListener(
          bridge.onEmit,
          [this] { broadcast(); },
          EA::Listener::Modes::TriggerOnEvent)
{
    stateListeners = attachStaticStateBinders(bridge);
    registerBuiltins();
    webView.addUserScript(bridgeShim, true);
    webView.addScriptMessageHandler(
        bridgeChannel, [this](const std::string& body) { onMessage(body); });
}

void WebViewBridge::registerBuiltins()
{
    // Native file drag-out as a first-class bridge command. The page invokes
    // `armFileDrag` with a DraggableFileList from a mousedown handler; Miro
    // deserializes the payload into the typed struct (no hand-rolled JSON),
    // and we arm the WebView so the next mouseDragged: starts the OS drag.
    using DraggableFileList = WebView::DraggableFileList;

    auto arm = std::function<void(const DraggableFileList&)> {
        [this](const DraggableFileList& list)
        {
            auto paths = std::vector<std::string> {};
            paths.reserve(list.files.size());

            for (const auto& file: list.files)
                paths.push_back(file.path);

            webView.armFileDrag(paths);
        }};

    bridge.on<DraggableFileList>("armFileDrag", arm);
}

WebViewBridge::~WebViewBridge()
{
    webView.removeScriptMessageHandler(bridgeChannel);
}

namespace
{
struct Envelope
{
    double id;
    std::string command;
    Miro::Json::Value payload;
};

std::optional<Envelope> parseEnvelope(const Miro::Json::Value& value)
{
    if (!value.isObject())
        return std::nullopt;

    auto& obj = value.asObject();

    auto idIt = obj.find("id");
    auto cmdIt = obj.find("command");
    auto payloadIt = obj.find("payload");

    if (idIt == obj.end() || cmdIt == obj.end() || !cmdIt->second.isString())
        return std::nullopt;

    auto envelope = Envelope {};
    envelope.id = idIt->second.isNumber() ? idIt->second.asNumber() : 0.0;
    envelope.command = cmdIt->second.asString();
    envelope.payload =
        payloadIt != obj.end() ? payloadIt->second : Miro::Json::Value {};
    return envelope;
}
} // namespace

void WebViewBridge::onMessage(const std::string& body)
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

    // A reply to a C++ -> page call (window.__eacp.callFunction) carries a
    // "reply" id; everything else is a command invocation from the page.
    if (handleCallReply(value))
        return;

    auto envelope = parseEnvelope(value);
    if (!envelope)
        return;

    auto id = envelope->id;

    // The C++ handlers are plain synchronous functions; the bridge is what
    // makes the call async. runCommand executes the Miro dispatch under the
    // configured execution mode (deferred on the main loop by default, or
    // on a worker thread) and yields an Async that settles on the main
    // thread; resolveWith then delivers the result back to the JS Promise
    // the shim is awaiting, keyed by the envelope id. Miro reports the
    // outcome (result or error) purely through the Resolve std::function —
    // it never touches the event loop.
    auto invoke = [this, command = envelope->command, payload = envelope->payload](
                      Miro::Resolve resolve)
    { bridge.dispatchAsync(command, payload, resolve); };

    auto work = runCommand(commandExecution, std::move(invoke));

    resolveWith(std::move(work),
                [this, id](const Miro::Json::Value& result, const std::string* error)
                { deliver(id, result, error); });
}

Threads::Async<Miro::Json::Value>
    WebViewBridge::call(const std::string& functionName,
                        const Miro::Json::Value& payload)
{
    auto id = ++callCounter;
    auto promise = Threads::AsyncPromise<Miro::Json::Value> {};
    pendingCalls.emplace(id, promise);

    auto payloadJson = Miro::Json::print(payload);
    if (payloadJson.empty())
        payloadJson = "null";

    auto script = std::string {"window.__eacp&&window.__eacp.callFunction("}
                  + std::to_string(static_cast<long long>(id)) + ","
                  + jsStringLiteral(functionName) + "," + payloadJson + ");";

    webView.evaluateJavaScript(script);

    return promise.get();
}

bool WebViewBridge::handleCallReply(const Miro::Json::Value& message)
{
    if (!message.isObject())
        return false;

    auto& obj = message.asObject();

    auto replyIt = obj.find("reply");
    if (replyIt == obj.end() || !replyIt->second.isNumber())
        return false;

    auto id = replyIt->second.asNumber();

    auto it = pendingCalls.find(id);
    if (it == pendingCalls.end())
        return true;

    auto promise = it->second;
    pendingCalls.erase(it);

    auto errorIt = obj.find("error");
    if (errorIt != obj.end() && errorIt->second.isString())
    {
        promise.reject(errorIt->second.asString());
        return true;
    }

    auto resultIt = obj.find("result");
    promise.resolve(resultIt != obj.end() ? resultIt->second : Miro::Json::Value {});
    return true;
}

void WebViewBridge::deliver(double id,
                            const Miro::Json::Value& result,
                            const std::string* error)
{
    auto resultJson = Miro::Json::print(result);

    if (resultJson.empty())
        resultJson = "null";

    auto errorJson = error ? jsStringLiteral(*error) : std::string {"null"};

    auto script = std::string {"window.__eacp&&window.__eacp.deliver("}
                  + std::to_string(static_cast<long long>(id)) + "," + resultJson
                  + "," + errorJson + ");";

    webView.evaluateJavaScript(script);
}

void WebViewBridge::broadcast()
{
    auto payloadJson = Miro::Json::print(bridge.currentPayload());

    if (payloadJson.empty())
        payloadJson = "null";

    auto script = std::string {"window.__eacp&&window.__eacp.dispatch("}
                  + jsStringLiteral(bridge.currentEvent()) + "," + payloadJson
                  + ");";

    webView.evaluateJavaScript(script);
}

} // namespace eacp::Graphics
