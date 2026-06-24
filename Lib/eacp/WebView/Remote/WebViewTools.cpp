#include "WebViewTools.h"

#include <eacp/Core/Utils/Logging.h>
#include <eacp/WebView/WebView.h>
#include <eacp/WebView/WebView/ElementIds.h>

#include <ResEmbed/ResEmbed.h>

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

Test::AppDriverOptions makeDriverOptions(const std::string& snapshotDir)
{
    auto driverOptions = Test::AppDriverOptions {};
    driverOptions.snapshotDir = snapshotDir;
    return driverOptions;
}

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

    return "Drives a live eacp WebView app. Capture what it shows with "
           "screenshot, or record a session with start_recording / "
           "stop_recording (window-level — page, GPU, native). Elements "
           "tagged with the '"
           + attribute
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

WebViewTools::WebViewTools(Graphics::WebView& webViewToUse,
                           Miro::Bridge& bridgeToUse,
                           std::string snapshotDir)
    : webView(webViewToUse)
    , driver(webViewToUse, bridgeToUse, makeDriverOptions(snapshotDir))
{
}

WebViewTools::~WebViewTools()
{
    webView.onNavigationFinished = std::move(previousFinishedHandler);
}

void WebViewTools::installConsoleCapture()
{
    auto source = loadConsoleCaptureSource();

    webView.addUserScript(source, true);

    // The user script misses a page that already loaded (or is mid
    // flight) when the tools attach; evaluating after each finished
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

void WebViewTools::registerTools(Graphics::Remote::WindowDebugServer& server)
{
    auto idAttribute = Graphics::ElementIds::attributeName();

    installConsoleCapture();

    // Layer the WebView's DOM-driving instructions over the window
    // server's capture ones.
    server.mcp().setInstructions(buildInstructions());

    server.addTool("page_info",
                   "Current page state: URL, title, load status, and the "
                   "configured element-id attribute.",
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

    server.addTool(
        "list_elements",
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

            // Built in the page and returned as one string rather
            // than via queryAll(): a large Vector<DomNode> (every
            // tagged element, with subtrees) trips a container bug
            // on some toolchains. The describe logic mirrors
            // describeNode().
            auto sel = Miro::Json::print(Miro::JSON {selector});
            auto idJs = Miro::Json::print(Miro::JSON {idAttribute});
            auto js =
                "(function(){var sel=" + sel + ",ID=" + idJs
                + ";function flat(s,n){s=(s||'').replace(/[\\n\\r\\t]/g,' ');"
                  "return s.length>n?s.slice(0,n)+'...':s;}"
                  "var els=document.querySelectorAll(sel);"
                  "var lines=[els.length+' element(s) matching '+sel];"
                  "for(var i=0;i<els.length;i++){var el=els[i];"
                  "var p=[el.tagName.toLowerCase()];"
                  "var a=el.getAttribute(ID);if(a)p.push('@'+a);"
                  "if(el.id)p.push('#'+el.id);"
                  "for(var j=0;j<el.classList.length;j++)p.push('.'+el.classList[j]);"
                  "var t=el.getAttribute('type');if(t)p.push('type='+t);"
                  "if(el.value)p.push('value=\"'+flat(String(el.value),40)+'\"');"
                  "if(el.checked)p.push('[checked]');"
                  "if(el.hasAttribute('disabled'))p.push('[disabled]');"
                  "var tx=flat((el.textContent||'').trim(),60);"
                  "if(tx)p.push('\"'+tx+'\"');"
                  "lines.push('- '+p.join(' '));}"
                  "return lines.join('\\n');})()";

            return MCP::toolText(driver.evaluate<std::string>(js));
        });

    server.addTool("click",
                   "Click the element matching the selector (fires mousedown, "
                   "mouseup, click).",
                   selectorSchema(),
                   [this](const Miro::JSON& args)
                   {
                       driver.click(requiredString(args, "selector"),
                                    callOptions(args));
                       return MCP::toolText("clicked");
                   });

    server.addTool("fill",
                   "Set a form control's value (React-aware) and fire "
                   "input/change events.",
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

    server.addTool("press",
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

    server.addTool("submit",
                   "Submit the form matching the selector.",
                   selectorSchema(),
                   [this](const Miro::JSON& args)
                   {
                       driver.submit(requiredString(args, "selector"),
                                     callOptions(args));
                       return MCP::toolText("submitted");
                   });

    server.addTool("text",
                   "Trimmed text content of the element matching the selector.",
                   selectorSchema(),
                   [this](const Miro::JSON& args)
                   {
                       return MCP::toolText(driver.text(
                           requiredString(args, "selector"), callOptions(args)));
                   });

    server.addTool("attr",
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

    server.addTool("count",
                   "Number of elements matching the selector.",
                   selectorSchema(),
                   [this](const Miro::JSON& args)
                   {
                       return MCP::toolText(std::to_string(driver.count(
                           requiredString(args, "selector"), callOptions(args))));
                   });

    server.addTool("wait_for",
                   "Poll until an element matching the selector exists (default "
                   "timeout 5000ms).",
                   R"({"type":"object","properties":{
                "selector":{"type":"string"},
                "timeout_ms":{"type":"number"}},
                "required":["selector"]})",
                   [this](const Miro::JSON& args)
                   {
                       driver.waitFor(requiredString(args, "selector"),
                                      callOptions(args));
                       return MCP::toolText("appeared");
                   });

    server.addTool(
        "dom",
        "Outer HTML of the matching element, or the whole document "
        "when no selector is given.",
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

    server.addTool("evaluate_js",
                   "Evaluate a JavaScript expression in the page and return its "
                   "JSON-serialised value.",
                   R"({"type":"object","properties":{
                "expression":{"type":"string"}},
                "required":["expression"]})",
                   [this](const Miro::JSON& args)
                   {
                       auto result = driver.evaluate(
                           requiredString(args, "expression"), callOptions(args));
                       return MCP::toolText(Miro::Json::print(result, 2));
                   });

    server.addTool(
        "console_logs",
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
                out += "\n(" + std::to_string(static_cast<int>(dropped.asNumber()))
                       + " earlier entries dropped)";

            return MCP::toolText(out);
        });

    server.addTool("invoke_command",
                   "Invoke a C++ bridge command directly (same handlers the "
                   "page calls) and return its JSON result.",
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

    server.addTool("navigate",
                   "Load a URL in the WebView.",
                   R"({"type":"object","properties":{
                "url":{"type":"string"}},
                "required":["url"]})",
                   [this](const Miro::JSON& args)
                   {
                       webView.loadURL(requiredString(args, "url"));
                       return MCP::toolText("navigation started");
                   });

    server.addTool("snapshot",
                   "Write <name>.html (full DOM) and <name>.png (in-process "
                   "page render) to the snapshot directory and return their "
                   "paths. Works headless, unlike the window-level screenshot.",
                   R"({"type":"object","properties":{
                "name":{"type":"string"}},
                "required":["name"]})",
                   [this](const Miro::JSON& args)
                   {
                       auto result = driver.snapshot(requiredString(args, "name"));
                       return MCP::toolText("dom: " + result.domPath
                                            + "\nscreenshot: "
                                            + result.screenshotPath);
                   });
}

} // namespace eacp::WebView::Remote
