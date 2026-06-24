#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Network/HTTPServer/HttpServer.h>

#include <Miro/Miro.h>

#include <functional>
#include <string>

namespace eacp::MCP
{

// One block of a tool result. type is "text" or "image"; build via
// textContent() / imageContent() rather than by hand.
struct ToolContent
{
    std::string type;
    std::string text;

    // Image payload, base64-encoded, plus its MIME type.
    std::string data;
    std::string mimeType;
};

ToolContent textContent(std::string text);
ToolContent imageContent(std::string base64Data, std::string mimeType);

struct ToolResult
{
    Vector<ToolContent> content;
    bool isError = false;
};

ToolResult toolText(std::string text);
ToolResult toolError(std::string message);

using ToolHandler = std::function<ToolResult(const Miro::JSON& args)>;

struct Tool
{
    std::string name;
    std::string description;

    // JSON Schema describing the arguments object.
    Miro::JSON inputSchema;
    ToolHandler handler;
};

// Minimal MCP (Model Context Protocol) server speaking the streamable
// HTTP transport in its stateless plain-JSON form: each JSON-RPC
// message arrives as one POST and is answered with a single
// application/json response (no SSE, no sessions). Implements
// initialize / ping / tools/list / tools/call — enough for any MCP
// client (Claude Code, MCP inspector, ...) to connect and call tools.
//
// Tool handlers run wherever the HTTP server dispatches its requests;
// mount on an HTTP::Server in EventLoop mode when handlers must touch
// main-thread-only state (WebView, bridge).
//
// Protocol errors (unknown method/tool, malformed params) become
// JSON-RPC error responses; exceptions thrown by a tool handler come
// back as a tool result with isError = true, which the calling agent
// sees as the tool failing rather than the transport.
class Server
{
public:
    Server(std::string serverName, std::string serverVersion);

    void addTool(Tool tool);

    // Optional server-level guidance returned from initialize —
    // clients surface it to the agent as usage instructions.
    void setInstructions(std::string text);

    // Routes POST (and a 405 for GET) at `path` to handle().
    void attach(HTTP::Server& server, const std::string& path = "/mcp");

    // Handles one transport POST directly — useful for tests and
    // custom mounting.
    HTTP::Response handle(const HTTP::Request& request);

private:
    Miro::JSON handleRequest(const std::string& method,
                             const Miro::JSON& params,
                             const Miro::JSON& id);
    Miro::JSON initializeResult(const Miro::JSON& params) const;
    Miro::JSON listToolsResult() const;
    Miro::JSON callToolResult(const Miro::JSON& params);

    std::string name;
    std::string version;
    std::string instructions;
    Vector<Tool> tools;
};

} // namespace eacp::MCP
