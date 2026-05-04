#include "Bridge.h"

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

std::string jsStringLiteral(const std::string& value)
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
{
    webView.addUserScript(bridgeShim, true);
    webView.addScriptMessageHandler(
        bridgeChannel, [this](const std::string& body) { onMessage(body); });
}

WebViewBridge::~WebViewBridge()
{
    webView.removeScriptMessageHandler(bridgeChannel);
}

void WebViewBridge::onMessage(const std::string& body)
{
    auto envelope = Miro::Json::Value {};

    try
    {
        envelope = Miro::Json::parse(body);
    }
    catch (const std::exception&)
    {
        return;
    }

    if (!envelope.isObject())
        return;

    auto& obj = envelope.asObject();

    auto idIt = obj.find("id");
    auto cmdIt = obj.find("command");
    auto payloadIt = obj.find("payload");

    if (idIt == obj.end() || cmdIt == obj.end() || !cmdIt->second.isString())
        return;

    auto id = idIt->second.isNumber() ? idIt->second.asNumber() : 0.0;
    auto command = cmdIt->second.asString();
    auto payload = payloadIt != obj.end() ? payloadIt->second : Miro::Json::Value {};

    try
    {
        auto result = commands.dispatch(command, payload);
        deliver(id, result, nullptr);
    }
    catch (const std::exception& e)
    {
        auto error = std::string {e.what()};
        deliver(id, Miro::Json::Value {}, &error);
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

void WebViewBridge::dispatchEvent(const std::string& event,
                                  const Miro::Json::Value& payload)
{
    auto payloadJson = Miro::Json::print(payload);

    if (payloadJson.empty())
        payloadJson = "null";

    auto script = std::string {"window.__eacp&&window.__eacp.dispatch("}
                  + jsStringLiteral(event) + "," + payloadJson + ");";

    webView.evaluateJavaScript(script);
}

} // namespace eacp::Graphics
