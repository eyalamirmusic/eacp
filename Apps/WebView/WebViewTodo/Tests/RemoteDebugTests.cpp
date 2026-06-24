#include "App.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Base64.h>
#include <eacp/Core/Utils/Containers.h>
#include <eacp/Graphics/Remote/WindowAutoAttach.h>
#include <eacp/Graphics/Remote/WindowDebugServer.h>
#include <eacp/Network/MCP/McpServer.h>
#include <eacp/WebView/Remote/WebViewTools.h>
#include <eacp/WebView/Test/TestApp.h>
#include <eacp/WebView/WebView/ElementIds.h>

#include <NanoTest/NanoTest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace eacp::WebView::Test;

using nano::check;

namespace
{

constexpr auto itemSelector = R"([data-testid="todo-item"])";
constexpr auto seededTodoCount = 3;

TestApp<MyApp>& testApp()
{
    // Same readiness gate as WebViewTodoTests.cpp — wait for every
    // seeded todo to render before a test body runs (see the comment
    // there for why the first item is not enough).
    static auto& instance = []() -> TestApp<MyApp>&
    {
        auto& self = createTestApp<MyApp>();
        return self.onReady([](AppDriver& driver)
                            { driver.waitForCount(itemSelector, seededTodoCount); });
    }();
    return instance;
}

MyApp& app()
{
    return testApp().app();
}

AppDriver& driver()
{
    return testApp().driver();
}

// Builds the unified debug server the way an app does: a window-level
// capture server (eacp-graphics-remote) enriched with this WebView's DOM
// tools. The window-level screenshot/recording need a real on-screen
// window + permission, so headless tests exercise the DOM half here; the
// capture half is verified live against real apps.
void addWebViewTools(eacp::Graphics::Remote::WindowDebugServer& server)
{
    auto tools = eacp::OwningPointer<eacp::Graphics::Remote::ServerExtension> {};
    tools.create<eacp::WebView::Remote::WebViewTools>(app().webView,
                                                      app().transport.getBridge());
    server.addExtension(std::move(tools));
}

HTTP::Request postJson(const std::string& body)
{
    auto request = HTTP::Request {"/mcp"};
    request.type = "POST";
    request.body = body;
    return request;
}

std::string rpcBody(int id, const std::string& method, const std::string& params)
{
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":")"
           + method + R"(","params":)" + params + "}";
}

std::string
    toolCallBody(int id, const std::string& tool, const std::string& argsJson = "{}")
{
    return rpcBody(id,
                   "tools/call",
                   R"({"name":")" + tool + R"(","arguments":)" + argsJson + "}");
}

Miro::JSON parseBody(const HTTP::Response& response)
{
    return Miro::Json::parse(response.content);
}

// A failing response should produce a readable error, not an
// out-of-bounds read: NanoTest's check() is non-fatal, so an
// unexpected shape would otherwise cascade into a segfault that hides
// what the server actually returned.
[[noreturn]] void failResponse(const char* what, const HTTP::Response& response)
{
    throw std::runtime_error(std::string {what} + " (status "
                             + std::to_string(response.statusCode)
                             + "): " + response.content);
}

Miro::JSON resultOf(const HTTP::Response& response)
{
    auto body = parseBody(response);
    if (!body.isObject() || !body.asObject().contains("result"))
        failResponse("response has no 'result'", response);
    return body.asObject().at("result");
}

Miro::JSON firstContent(const HTTP::Response& response)
{
    auto result = resultOf(response);
    auto it = result.asObject().find("content");
    if (it == result.asObject().end() || !it->second.isArray()
        || it->second.asArray().empty())
        failResponse("response has no content", response);
    return it->second.asArray()[0];
}

std::string firstText(const HTTP::Response& response)
{
    auto content = firstContent(response);
    if (!content.isObject() || !content.asObject().contains("text"))
        failResponse("content block has no text", response);
    return content.asObject().at("text").asString();
}

bool isToolError(const HTTP::Response& response)
{
    return resultOf(response).asObject().at("isError").asBool();
}

} // namespace

// --- ElementIds: pure unit tests, no fixture needed (nano::test
// directly, so no app restart per test).

auto tSelectorResolution = nano::test("Remote/elementIdSelectorResolution") = []
{
    namespace Ids = eacp::Graphics::ElementIds;

    check(Ids::attributeName() == "data-testid");
    check(Ids::selectorFor("todo-add") == R"([data-testid="todo-add"])");

    check(Ids::resolveSelector("@todo-list li")
          == R"([data-testid="todo-list"] li)");
    check(Ids::resolveSelector("@todo-item.done")
          == R"([data-testid="todo-item"].done)");
    check(Ids::resolveSelector("@todo-item:last-child @todo-text")
          == R"([data-testid="todo-item"]:last-child [data-testid="todo-text"])");

    // Pass-through cases: no @, @ mid-token, @ inside attribute values.
    check(Ids::resolveSelector("ul.list > li") == "ul.list > li");
    check(Ids::resolveSelector("a@b") == "a@b");
    check(Ids::resolveSelector(R"([href="mailto:a @b"])")
          == R"([href="mailto:a @b"])");
};

auto tDebugPortPolicy = nano::test("Remote/debugPortPolicy") = []
{
    // The port policy lives with the window server now (the listen port
    // is window-level, shared by the DOM tools layered on top).
    namespace Remote = eacp::Graphics::Remote;

    // Unset -> the well-known default; "off" -> disabled; a number ->
    // that port (0 = ephemeral); garbage -> the default.
    check(Remote::resolveDebugPort(std::nullopt) == Remote::defaultDebugPort);
    check(Remote::resolveDebugPort("off") == std::nullopt);
    check(Remote::resolveDebugPort("OFF") == std::nullopt);
    check(Remote::resolveDebugPort("4242") == 4242);
    check(Remote::resolveDebugPort("0") == 0);
    check(Remote::resolveDebugPort("nonsense") == Remote::defaultDebugPort);
};

auto tBase64Encoding = nano::test("Remote/base64Encoding") = []
{
    check(eacp::Base64::encode("") == "");
    check(eacp::Base64::encode("M") == "TQ==");
    check(eacp::Base64::encode("Ma") == "TWE=");
    check(eacp::Base64::encode("Man") == "TWFu");
    check(eacp::Base64::encode("light work.") == "bGlnaHQgd29yay4=");
};

// --- MCP::Server: protocol-level tests against a standalone server
// with a stub tool — no app, no sockets.

namespace
{

MCP::Server makeEchoServer()
{
    auto server = MCP::Server {"test-server", "0.1"};

    server.addTool(
        {"echo",
         "Echoes the msg argument",
         Miro::Json::parse(R"({"type":"object"})"),
         [](const Miro::JSON& args)
         { return MCP::toolText("echo:" + args.asObject().at("msg").asString()); }});

    server.addTool({"explode",
                    "Always throws",
                    Miro::Json::parse(R"({"type":"object"})"),
                    [](const Miro::JSON&) -> MCP::ToolResult
                    { throw std::runtime_error("boom"); }});

    return server;
}

} // namespace

auto tMcpInitialize = nano::test("Remote/mcpInitializeHandshake") = []
{
    auto server = makeEchoServer();

    auto response = server.handle(postJson(rpcBody(
        1, "initialize", R"({"protocolVersion":"2025-06-18","capabilities":{}})")));

    check(response.statusCode == 200);

    auto result = resultOf(response).asObject();
    check(result.at("protocolVersion").asString() == "2025-06-18");
    check(result.at("serverInfo").asObject().at("name").asString() == "test-server");
    check(result.at("capabilities").asObject().contains("tools"));

    // Notifications are acknowledged with 202 and no body.
    auto note = server.handle(
        postJson(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    check(note.statusCode == 202);
};

auto tMcpListAndCall = nano::test("Remote/mcpListsAndCallsTools") = []
{
    auto server = makeEchoServer();

    auto listResponse = server.handle(postJson(rpcBody(2, "tools/list", "{}")));
    auto list = resultOf(listResponse);
    auto& tools = list.asObject().at("tools").asArray();
    if (tools.size() != 2)
        failResponse("tools/list did not return 2 tools", listResponse);
    check(tools[0].asObject().at("name").asString() == "echo");

    auto call = server.handle(postJson(toolCallBody(3, "echo", R"({"msg":"hi"})")));
    check(firstText(call) == "echo:hi");
    check(!isToolError(call));
};

auto tMcpErrors = nano::test("Remote/mcpErrorMapping") = []
{
    auto server = makeEchoServer();

    auto unknownMethod = server.handle(postJson(rpcBody(4, "resources/list", "{}")));
    auto methodError = parseBody(unknownMethod).asObject().at("error");
    check(static_cast<int>(methodError.asObject().at("code").asNumber()) == -32601);

    auto unknownTool = server.handle(postJson(toolCallBody(5, "nope")));
    auto toolError = parseBody(unknownTool).asObject().at("error");
    check(static_cast<int>(toolError.asObject().at("code").asNumber()) == -32602);

    // A tool that throws is a tool-level failure, not a protocol error.
    auto exploded = server.handle(postJson(toolCallBody(6, "explode")));
    check(isToolError(exploded));
    check(firstText(exploded) == "boom");

    auto badJson = server.handle(postJson("{not json"));
    auto parseError = parseBody(badJson).asObject().at("error");
    check(static_cast<int>(parseError.asObject().at("code").asNumber()) == -32700);
};

// --- @id shorthand through the live app.

auto tDriverAtShorthand = test("Remote/driverResolvesAtShorthand") = []
{
    check(driver().count("@todo-item") == seededTodoCount);

    driver().fill("@todo-input", "From @ selector");
    driver().click("@todo-add");

    driver().waitForCount("@todo-item", seededTodoCount + 1);
    check(driver().text("@todo-item:last-child @todo-text") == "From @ selector");
};

auto tDomNodeAtShorthand = test("Remote/domNodeResolvesAtShorthand") = []
{
    auto list = driver().query("@todo-list");

    check(list.findAll("@todo-toggle").size() == seededTodoCount);
    check(list.find("@todo-remove").attr("aria-label") == "Remove");
};

// --- Window debug server + WebView DOM tools: full tool wiring against
// the live app, calling the transport entry point directly (no sockets).

auto tDebugServerDrivesApp = test("Remote/debugServerDrivesAppOverMcp") = []
{
    auto server =
        eacp::Graphics::Remote::WindowDebugServer {app().window, {.port = 0}};
    addWebViewTools(server);

    auto count = server.handleMcp(
        postJson(toolCallBody(1, "count", R"({"selector":"@todo-item"})")));
    check(firstText(count) == "3");

    auto listed = server.handleMcp(postJson(toolCallBody(2, "list_elements")));
    check(firstText(listed).find("@todo-add") != std::string::npos);

    server.handleMcp(postJson(toolCallBody(
        3, "fill", R"({"selector":"@todo-input","value":"Added over MCP"})")));
    server.handleMcp(
        postJson(toolCallBody(4, "click", R"({"selector":"@todo-add"})")));

    driver().waitForCount("@todo-item", 4);
    check(driver().text("@todo-item:last-child @todo-text") == "Added over MCP");

    auto invoked = server.handleMcp(
        postJson(toolCallBody(5, "invoke_command", R"({"command":"getTodos"})")));
    check(firstText(invoked).find("Added over MCP") != std::string::npos);

    auto missing = server.handleMcp(
        postJson(toolCallBody(6, "click", R"({"selector":"@no-such-element"})")));
    check(isToolError(missing));
};

auto tDebugServerSnapshotAndLogs =
    test("Remote/debugServerSnapshotAndConsoleLogs") = []
{
    auto server =
        eacp::Graphics::Remote::WindowDebugServer {app().window, {.port = 0}};
    addWebViewTools(server);

    // snapshot is the in-process page render — headless-friendly, unlike
    // the window-level SCK screenshot (which needs a visible window +
    // Screen Recording permission, verified live against real apps).
    auto snap = server.handleMcp(
        postJson(toolCallBody(1, "snapshot", R"({"name":"remote-test"})")));
    check(firstText(snap).find(".png") != std::string::npos);

    driver().evaluate("(console.log('mcp-console-probe'), true)");

    auto logs = server.handleMcp(
        postJson(toolCallBody(2, "console_logs", R"({"clear":true})")));
    check(firstText(logs).find("mcp-console-probe") != std::string::npos);

    auto info = server.handleMcp(postJson(toolCallBody(3, "page_info")));
    check(firstText(info).find("elementIdAttribute: data-testid")
          != std::string::npos);
};

// --- DebugServer over a real socket: an MCP client posting from
// another thread while the app's event loop pumps.

auto tDebugServerOverHttp = test("Remote/debugServerAnswersOverRealHttp") = []
{
    auto server =
        eacp::Graphics::Remote::WindowDebugServer {app().window, {.port = 0}};
    addWebViewTools(server);
    check(server.port() > 0);

    auto url = "http://127.0.0.1:" + std::to_string(server.port()) + "/mcp";

    auto done = std::atomic<bool> {false};
    auto response = HTTP::Response {};

    auto worker = std::thread {
        [&]
        {
            auto request = HTTP::Request::post(
                url, toolCallBody(1, "count", R"({"selector":"@todo-item"})"));
            request.headers["Content-Type"] = "application/json";
            response = request.perform();
            done = true;
        }};

    // The request handler is dispatched onto this (main) thread, so
    // pump the loop until the background client has its answer.
    auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!done && std::chrono::steady_clock::now() < deadline)
        Threads::runEventLoopFor(20ms);

    worker.join();

    check(done);
    check(response.statusCode == 200);
    check(firstText(response) == "3");
};
