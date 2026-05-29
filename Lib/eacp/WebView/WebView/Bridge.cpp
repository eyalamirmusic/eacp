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
        }
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
    , emitListener(bridge.onEmit,
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

std::optional<Envelope> parseEnvelope(const std::string& body)
{
    auto value = Miro::Json::Value {};

    try
    {
        value = Miro::Json::parse(body);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

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
    auto envelope = parseEnvelope(body);
    if (!envelope)
        return;

    try
    {
        auto result = bridge.dispatch(envelope->command, envelope->payload);
        deliver(envelope->id, result, nullptr);
    }
    catch (const std::exception& e)
    {
        auto error = std::string {e.what()};
        deliver(envelope->id, Miro::Json::Value {}, &error);
    }
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
                  + jsStringLiteral(bridge.currentEvent()) + "," + payloadJson + ");";

    webView.evaluateJavaScript(script);
}

} // namespace eacp::Graphics
