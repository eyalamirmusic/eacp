#include "Bridge.h"

#include "ElementIds.h"
#include "JsStringLiteral.h"
#include "StateBridge.h"

#include <eacp/Core/App/AppEnvironment.h>

#include <ResEmbed/ResEmbed.h>

#include <optional>
#include <stdexcept>

namespace eacp::Graphics
{
namespace
{
constexpr const char* bridgeChannel = "__eacpBridge";

// The bidirectional JSON-RPC bridge installed into every page as a user
// script. Authored as a real .js file (Resources/bridge-shim.js) and
// embedded via ResEmbed so it gets editor tooling and lives outside the
// C++ string-literal escaping rules.
std::string loadBridgeShim()
{
    auto view = ResEmbed::get("bridge-shim.js", "EacpWebView");
    if (!view)
        throw std::runtime_error(
            "eacp-webview: embedded bridge-shim.js resource not found");
    return view.toString();
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
    webView.addUserScript(loadBridgeShim(), true);

    // Page code (event tracking, dev tooling) reads the configured
    // ElementIds attribute from here rather than hardcoding it.
    webView.addUserScript("window.__eacpElementIdAttribute = "
                              + jsStringLiteral(ElementIds::attributeName()) + ";",
                          true);

    // Developer affordance: when the build linked a debug transport
    // (see DebugAttach.h), attach it to this WebView + bridge pair.
    // Headless runs (test fixtures) drive the WebView in-process and
    // don't want servers spawning per fixture rebuild.
    if (auto& factory = Detail::debugAttachFactory();
        factory && !Apps::getAppEnvironment().headless)
        debugAttachment = factory(webView, bridge);
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
            auto paths = Vector<std::string> {};
            paths.reserveAtLeast(list.files.size());

            for (const auto& file: list.files)
                paths.add(file.path);

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

    auto mode = commandExecution;

    if (auto it = commandModes.find(envelope->command); it != commandModes.end())
        mode = it->second;

    auto work = runCommand(mode, std::move(invoke));

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
