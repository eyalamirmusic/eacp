#include <eacp/Network/MCP/McpServer.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;

namespace
{

MCP::Tool echoTool(std::string name)
{
    return {std::move(name),
            "echoes",
            Miro::Json::parse(R"({"type":"object","properties":{}})"),
            [](const Miro::JSON&) { return MCP::toolText("ok"); }};
}

HTTP::Request post(const std::string& body)
{
    auto request = HTTP::Request {};
    request.body = body;
    return request;
}

Miro::JSON callResult(MCP::Server& server, const std::string& body)
{
    auto response = server.handle(post(body));
    return Miro::Json::parse(response.content);
}

} // namespace

// tools/list returns a multi-element array of tool objects nested in the
// result object — the exact shape commit 446823e hand-assembled as a string
// to dodge a crash it attributed to Miro::JSON. We build it the natural way
// (nested values) and read the array back, which is where the segfault was
// claimed. No crash, correct contents -> the workaround was unnecessary.
auto tListsMultipleTools = test("MCP/toolsListMultiElementArray") = []
{
    auto server = MCP::Server {"test", "1.0"};
    server.addTool(echoTool("alpha"));
    server.addTool(echoTool("beta"));
    server.addTool(echoTool("gamma"));

    auto parsed = callResult(
        server, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    const auto& tools = parsed["result"].asObject().at("tools").asArray();
    check(tools.size() == 3);
    check(tools[0].asObject().at("name").asString() == "alpha");
    check(tools[2].asObject().at("name").asString() == "gamma");
};

// A tool result carrying an image block AND a text block is a multi-element
// content array nested in the result — the second shape the workaround
// avoided. Build it naturally, read both blocks back.
auto tImageAndTextResult = test("MCP/toolCallImagePlusTextContent") = []
{
    auto server = MCP::Server {"test", "1.0"};
    server.addTool({"shot",
                    "captures",
                    Miro::Json::parse(R"({"type":"object","properties":{}})"),
                    [](const Miro::JSON&)
                    {
                        auto result = MCP::ToolResult {};
                        result.content.add(MCP::imageContent("AAAA", "image/png"));
                        result.content.add(MCP::textContent("100x100 PNG"));
                        return result;
                    }});

    auto parsed = callResult(
        server,
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call",)"
        R"("params":{"name":"shot","arguments":{}}})");

    const auto& content = parsed["result"].asObject().at("content").asArray();
    check(content.size() == 2);
    check(content[0].asObject().at("type").asString() == "image");
    check(content[0].asObject().at("mimeType").asString() == "image/png");
    check(content[1].asObject().at("type").asString() == "text");
    check(content[1].asObject().at("text").asString() == "100x100 PNG");
};

auto tUnknownMethodErrors = test("MCP/unknownMethodIsJsonRpcError") = []
{
    auto server = MCP::Server {"test", "1.0"};

    auto parsed =
        callResult(server, R"({"jsonrpc":"2.0","id":3,"method":"nope"})");

    check(parsed["error"].asObject().at("code").asNumber() == -32601);
};
