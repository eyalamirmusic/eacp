#include "DebugServer.h"

#include <eacp/Core/Utils/Base64.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/WebView/WebView.h>
#include <eacp/WebView/WebView/ElementIds.h>

#include <ResEmbed/ResEmbed.h>

#include <span>
#include <stdexcept>
#include <utility>

namespace eacp::WebView::Remote
{

namespace
{

constexpr auto maxDomChars = std::size_t {200000};

std::string loadConsoleCaptureSource()
{
    auto view = ResEmbed::get("console-capture.js", "RemoteAgent");
    if (!view)
        throw std::runtime_error("eacp-webview-remote: embedded "
                                 "console-capture.js resource not found");
    return view.toString();
}

Test::AppDriverOptions makeDriverOptions(const DebugServerOptions& options)
{
    auto driverOptions = Test::AppDriverOptions {};
    driverOptions.snapshotDir = options.snapshotDir;
    return driverOptions;
}

struct BusyReset
{
    bool& flag;

    ~BusyReset() { flag = false; }
};

Miro::JSON field(const Miro::JSON& object, const std::string& key)
{
    if (!object.isObject())
        return {};

    auto& obj = object.asObject();
    auto it = obj.find(key);
    return it != obj.end() ? it->second : Miro::JSON {};
}

std::string requiredString(const Miro::JSON& args, const std::string& key)
{
    auto value = field(args, key);
    if (!value.isString())
        throw std::runtime_error("missing required string argument: " + key);
    return value.asString();
}

std::string optionalString(const Miro::JSON& args,
                           const std::string& key,
                           const std::string& fallback = {})
{
    auto value = field(args, key);
    return value.isString() ? value.asString() : fallback;
}

bool optionalBool(const Miro::JSON& args, const std::string& key)
{
    auto value = field(args, key);
    return value.isBool() && value.asBool();
}

Test::CallOptions callOptions(const Miro::JSON& args)
{
    auto opts = Test::CallOptions {};

    auto timeout = field(args, "timeout_ms");
    if (timeout.isNumber())
        opts.timeoutMs = static_cast<int>(timeout.asNumber());

    return opts;
}

std::string flattened(std::string text, std::size_t maxLength)
{
    for (auto& c: text)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';

    if (text.size() > maxLength)
        return text.substr(0, maxLength) + "...";

    return text;
}

std::string describeNode(const Test::DomNode& node, const std::string& idAttribute)
{
    auto out = node.tag();

    if (auto id = node.attr(idAttribute))
        out += " @" + *id;

    if (auto htmlId = node.attr("id"))
        out += " #" + *htmlId;

    for (const auto& className: node.classes())
        out += " ." + className;

    if (auto type = node.attr("type"))
        out += " type=" + *type;

    if (!node.value.empty())
        out += " value=\"" + flattened(node.value, 40) + "\"";

    if (node.checked)
        out += " [checked]";

    if (node.hasAttr("disabled"))
        out += " [disabled]";

    auto text = flattened(node.text(), 60);
    if (!text.empty())
        out += " \"" + text + "\"";

    return out;
}

std::string selectorSchema()
{
    return R"({"type":"object","properties":{
        "selector":{"type":"string",
            "description":"CSS selector; @someId targets the element-id attribute"}},
        "required":["selector"]})";
}

std::string emptySchema()
{
    return R"({"type":"object","properties":{}})";
}

std::string buildInstructions()
{
    auto attribute = Graphics::ElementIds::attributeName();

    return "Drives a live eacp WebView app. Elements tagged with the '" + attribute
           + "' attribute are the stable automation handles; every selector "
             "argument accepts the @id shorthand (\"@add-todo\" means ["
           + attribute
           + "=\"add-todo\"]) alongside regular CSS. Orient with page_info, "
             "list_elements and screenshot; read app output with "
             "console_logs; interact with click / fill / press / submit; "
             "reach native C++ handlers directly with invoke_command. Tool "
             "calls pump the app's event loop — issue them one at a time.";
}

} // namespace

DebugServer::DebugServer(Graphics::WebView& webViewToUse,
                         Miro::Bridge& bridgeToUse,
                         DebugServerOptions options)
    : webView(webViewToUse)
    , driver(webViewToUse, bridgeToUse, makeDriverOptions(options))
    , mcp("eacp-debug-server", "1.0.0")
{
    installConsoleCapture();
    registerTools();

    mcp.setInstructions(buildInstructions());
    mcp.attach(http);
    rpc.create(http, bridgeToUse);

    if (!http.listen(options.port))
        throw std::runtime_error("DebugServer: failed to listen on port "
                                 + std::to_string(options.port));

    LOG("DebugServer: MCP endpoint at http://127.0.0.1:"
        + std::to_string(http.boundPort()) + "/mcp");
}

DebugServer::~DebugServer()
{
    http.stop();
    webView.onNavigationFinished = std::move(previousFinishedHandler);
}

int DebugServer::port() const
{
    return http.boundPort();
}

HTTP::Response DebugServer::handleMcp(const HTTP::Request& request)
{
    return mcp.handle(request);
}

void DebugServer::installConsoleCapture()
{
    auto source = loadConsoleCaptureSource();

    webView.addUserScript(source, true);

    // The user script misses a page that already loaded (or is mid
    // flight) when the server attaches; evaluating after each finished
    // navigation — and immediately for a late attach — closes the gap.
    // The capture script is idempotent, so double-installs are free.
    previousFinishedHandler = webView.onNavigationFinished;
    webView.onNavigationFinished =
        [this, source, previous = previousFinishedHandler](const std::string& url)
    {
        if (previous)
            previous(url);

        webView.evaluateJavaScript(source);
    };

    if (!webView.isLoading() && !webView.getURL().empty())
        webView.evaluateJavaScript(source);
}

void DebugServer::addTool(std::string name,
                          std::string description,
                          const std::string& schemaJson,
                          MCP::ToolHandler handler)
{
    // Tool calls run nested event loops (AppDriver pumps while waiting
    // on the page); a second call arriving mid-pump would re-enter the
    // driver, so it gets refused instead.
    auto guarded = [this, handler = std::move(handler)](const Miro::JSON& args)
    {
        if (busy)
            return MCP::toolError("another tool call is still executing; "
                                  "issue tool calls one at a time");

        busy = true;
        auto reset = BusyReset {busy};
        return handler(args);
    };

    mcp.addTool({std::move(name),
                 std::move(description),
                 Miro::Json::parse(schemaJson),
                 std::move(guarded)});
}

void DebugServer::registerTools()
{
    auto idAttribute = Graphics::ElementIds::attributeName();

    addTool("page_info",
            "Current page state: URL, title, load status, and the configured "
            "element-id attribute.",
            emptySchema(),
            [this, idAttribute](const Miro::JSON&)
            {
                auto info = std::string {};
                info += "url: " + webView.getURL() + "\n";
                info += "title: " + webView.getTitle() + "\n";
                info += "loading: "
                        + std::string {webView.isLoading() ? "true" : "false"}
                        + "\n";
                info += "elementIdAttribute: " + idAttribute;
                return MCP::toolText(info);
            });

    addTool("list_elements",
            "List elements matching a selector, one line each: tag, @id, "
            "classes, value/checked state, and text. Defaults to every "
            "element tagged with the '"
                + idAttribute + "' attribute — the page's automation handles.",
            R"({"type":"object","properties":{
                "selector":{"type":"string",
                    "description":"CSS selector; defaults to all tagged elements"}}})",
            [this, idAttribute](const Miro::JSON& args)
            {
                auto selector =
                    optionalString(args, "selector", "[" + idAttribute + "]");
                auto nodes = driver.queryAll(selector);

                auto out = std::to_string(nodes.size()) + " element(s) matching "
                           + selector;
                for (const auto& node: nodes)
                    out += "\n- " + describeNode(node, idAttribute);

                return MCP::toolText(out);
            });

    addTool("click",
            "Click the element matching the selector (fires mousedown, "
            "mouseup, click).",
            selectorSchema(),
            [this](const Miro::JSON& args)
            {
                driver.click(requiredString(args, "selector"), callOptions(args));
                return MCP::toolText("clicked");
            });

    addTool("fill",
            "Set a form control's value (React-aware) and fire input/change "
            "events.",
            R"({"type":"object","properties":{
                "selector":{"type":"string"},
                "value":{"type":"string"}},
                "required":["selector","value"]})",
            [this](const Miro::JSON& args)
            {
                driver.fill(requiredString(args, "selector"),
                            requiredString(args, "value"),
                            callOptions(args));
                return MCP::toolText("filled");
            });

    addTool("press",
            "Send a keydown/keyup pair to the element (key as in "
            "KeyboardEvent.key, e.g. \"Enter\").",
            R"({"type":"object","properties":{
                "selector":{"type":"string"},
                "key":{"type":"string"}},
                "required":["selector","key"]})",
            [this](const Miro::JSON& args)
            {
                driver.press(requiredString(args, "selector"),
                             requiredString(args, "key"),
                             callOptions(args));
                return MCP::toolText("pressed");
            });

    addTool("submit",
            "Submit the form matching the selector.",
            selectorSchema(),
            [this](const Miro::JSON& args)
            {
                driver.submit(requiredString(args, "selector"), callOptions(args));
                return MCP::toolText("submitted");
            });

    addTool("text",
            "Trimmed text content of the element matching the selector.",
            selectorSchema(),
            [this](const Miro::JSON& args)
            {
                return MCP::toolText(driver.text(requiredString(args, "selector"),
                                                 callOptions(args)));
            });

    addTool("attr",
            "Read an attribute from the element matching the selector.",
            R"({"type":"object","properties":{
                "selector":{"type":"string"},
                "name":{"type":"string"}},
                "required":["selector","name"]})",
            [this](const Miro::JSON& args)
            {
                auto value = driver.attr(requiredString(args, "selector"),
                                         requiredString(args, "name"),
                                         callOptions(args));
                return MCP::toolText(value ? *value : "(attribute absent)");
            });

    addTool("count",
            "Number of elements matching the selector.",
            selectorSchema(),
            [this](const Miro::JSON& args)
            {
                return MCP::toolText(std::to_string(driver.count(
                    requiredString(args, "selector"), callOptions(args))));
            });

    addTool("wait_for",
            "Poll until an element matching the selector exists (default "
            "timeout 5000ms).",
            R"({"type":"object","properties":{
                "selector":{"type":"string"},
                "timeout_ms":{"type":"number"}},
                "required":["selector"]})",
            [this](const Miro::JSON& args)
            {
                driver.waitFor(requiredString(args, "selector"), callOptions(args));
                return MCP::toolText("appeared");
            });

    addTool("dom",
            "Outer HTML of the matching element, or the whole document when "
            "no selector is given.",
            R"({"type":"object","properties":{"selector":{"type":"string"}}})",
            [this](const Miro::JSON& args)
            {
                auto html =
                    driver.dom(optionalString(args, "selector"), callOptions(args));

                if (html.size() > maxDomChars)
                    html = html.substr(0, maxDomChars) + "\n... (truncated, "
                           + std::to_string(html.size())
                           + " chars total — narrow with a selector)";

                return MCP::toolText(html);
            });

    addTool("evaluate_js",
            "Evaluate a JavaScript expression in the page and return its "
            "JSON-serialised value.",
            R"({"type":"object","properties":{
                "expression":{"type":"string"}},
                "required":["expression"]})",
            [this](const Miro::JSON& args)
            {
                auto result = driver.evaluate(requiredString(args, "expression"),
                                              callOptions(args));
                return MCP::toolText(Miro::Json::print(result, 2));
            });

    addTool("screenshot",
            "Capture the rendered page as a PNG image.",
            emptySchema(),
            [this](const Miro::JSON&)
            {
                auto shot = driver.screenshot();
                auto png = shot.image.toPng();

                auto result = MCP::ToolResult {};
                result.content.add(MCP::imageContent(
                    Base64::encode(std::span {png.data(),
                                              static_cast<std::size_t>(png.size())}),
                    "image/png"));
                result.content.add(MCP::textContent(
                    std::to_string(shot.image.width()) + "x"
                    + std::to_string(shot.image.height()) + " PNG"));
                return result;
            });

    addTool("snapshot",
            "Write <name>.html (full DOM) and <name>.png (screenshot) to the "
            "snapshot directory and return their paths.",
            R"({"type":"object","properties":{
                "name":{"type":"string"}},
                "required":["name"]})",
            [this](const Miro::JSON& args)
            {
                auto result = driver.snapshot(requiredString(args, "name"));
                return MCP::toolText("dom: " + result.domPath
                                     + "\nscreenshot: " + result.screenshotPath);
            });

    addTool("console_logs",
            "Console output, uncaught errors, and unhandled rejections "
            "captured from the page. Pass clear=true to flush the buffer.",
            R"({"type":"object","properties":{"clear":{"type":"boolean"}}})",
            [this](const Miro::JSON& args)
            {
                auto clear = optionalBool(args, "clear");
                auto drained = driver.evaluate(
                    std::string {"window.__eacpConsole ? "
                                 "window.__eacpConsole.drain("}
                    + (clear ? "true" : "false") + ") : {entries: [], dropped: 0}");

                auto entries = field(drained, "entries");
                if (!entries.isArray() || entries.asArray().empty())
                    return MCP::toolText("(no console output captured)");

                auto out = std::string {};
                for (const auto& entry: entries.asArray())
                {
                    if (!out.empty())
                        out += "\n";
                    out += "[" + optionalString(entry, "level", "?") + "] "
                           + optionalString(entry, "message");
                }

                auto dropped = field(drained, "dropped");
                if (dropped.isNumber() && dropped.asNumber() > 0)
                    out += "\n("
                           + std::to_string(static_cast<int>(dropped.asNumber()))
                           + " earlier entries dropped)";

                return MCP::toolText(out);
            });

    addTool("invoke_command",
            "Invoke a C++ bridge command directly (same handlers the page "
            "calls) and return its JSON result.",
            R"({"type":"object","properties":{
                "command":{"type":"string"},
                "payload":{"type":"object"}},
                "required":["command"]})",
            [this](const Miro::JSON& args)
            {
                auto payload = field(args, "payload");
                if (payload.isNull())
                    payload = Miro::JSON {Miro::Json::Object {}};

                auto result =
                    driver.invoke(requiredString(args, "command"), payload);
                return MCP::toolText(Miro::Json::print(result, 2));
            });

    addTool("navigate",
            "Load a URL in the WebView.",
            R"({"type":"object","properties":{
                "url":{"type":"string"}},
                "required":["url"]})",
            [this](const Miro::JSON& args)
            {
                webView.loadURL(requiredString(args, "url"));
                return MCP::toolText("navigation started");
            });
}

} // namespace eacp::WebView::Remote
