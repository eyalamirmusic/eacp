#include "McpServer.h"

#include <stdexcept>
#include <utility>

namespace eacp::MCP
{

namespace
{

constexpr auto protocolVersion = "2025-03-26";

constexpr auto parseErrorCode = -32700;
constexpr auto invalidRequestCode = -32600;
constexpr auto methodNotFoundCode = -32601;
constexpr auto invalidParamsCode = -32602;

Miro::JSON makeObject()
{
    return Miro::JSON {Miro::Json::Object {}};
}

HTTP::Response jsonResponse(const Miro::JSON& body)
{
    auto response = HTTP::Response {};
    response.statusCode = 200;
    response.setContent(Miro::Json::print(body), "application/json");
    return response;
}

// Notifications and client-side responses get no JSON-RPC reply —
// the transport acknowledges them with 202 Accepted.
HTTP::Response acceptedResponse()
{
    auto response = HTTP::Response {};
    response.statusCode = 202;
    return response;
}

Miro::JSON envelope(const Miro::JSON& id)
{
    auto body = makeObject();
    body.asObject()["jsonrpc"] = Miro::JSON {"2.0"};
    body.asObject()["id"] = id;
    return body;
}

Miro::JSON errorEnvelope(const Miro::JSON& id, int code, const std::string& message)
{
    auto error = makeObject();
    error.asObject()["code"] = Miro::JSON {code};
    error.asObject()["message"] = Miro::JSON {message};

    auto body = envelope(id);
    body.asObject()["error"] = error;
    return body;
}

Miro::JSON resultEnvelope(const Miro::JSON& id, const Miro::JSON& result)
{
    auto body = envelope(id);
    body.asObject()["result"] = result;
    return body;
}

Miro::JSON field(const Miro::JSON& object, const std::string& key)
{
    if (!object.isObject())
        return {};

    auto& obj = object.asObject();
    auto it = obj.find(key);
    return it != obj.end() ? it->second : Miro::JSON {};
}

Miro::JSON contentToJson(const ToolContent& content)
{
    auto json = makeObject();
    json.asObject()["type"] = Miro::JSON {content.type};

    if (content.type == "image")
    {
        json.asObject()["data"] = Miro::JSON {content.data};
        json.asObject()["mimeType"] = Miro::JSON {content.mimeType};
    }
    else
    {
        json.asObject()["text"] = Miro::JSON {content.text};
    }

    return json;
}

Miro::JSON resultToJson(const ToolResult& result)
{
    auto content = Miro::Json::Array {};
    for (const auto& block: result.content)
        content.add(contentToJson(block));

    auto json = makeObject();
    json.asObject()["content"] = Miro::JSON {std::move(content)};
    json.asObject()["isError"] = Miro::JSON {result.isError};
    return json;
}

} // namespace

ToolContent textContent(std::string text)
{
    auto content = ToolContent {};
    content.type = "text";
    content.text = std::move(text);
    return content;
}

ToolContent imageContent(std::string base64Data, std::string mimeType)
{
    auto content = ToolContent {};
    content.type = "image";
    content.data = std::move(base64Data);
    content.mimeType = std::move(mimeType);
    return content;
}

ToolResult toolText(std::string text)
{
    auto result = ToolResult {};
    result.content.add(textContent(std::move(text)));
    return result;
}

ToolResult toolError(std::string message)
{
    auto result = toolText(std::move(message));
    result.isError = true;
    return result;
}

Server::Server(std::string serverName, std::string serverVersion)
    : name(std::move(serverName))
    , version(std::move(serverVersion))
{
}

void Server::addTool(Tool tool)
{
    tools.add(std::move(tool));
}

void Server::setInstructions(std::string text)
{
    instructions = std::move(text);
}

void Server::attach(HTTP::Server& server, const std::string& path)
{
    server.post(path, [this](const HTTP::Request& req) { return handle(req); });

    server.get(path,
               [](const HTTP::Request&)
               {
                   auto response = HTTP::Response {};
                   response.statusCode = 405;
                   response.setHeader("Allow", "POST");
                   response.setContent("MCP endpoint: POST JSON-RPC messages "
                                       "here",
                                       "text/plain");
                   return response;
               });
}

HTTP::Response Server::handle(const HTTP::Request& request)
{
    auto message = Miro::JSON {};
    try
    {
        message = Miro::Json::parse(request.body);
    }
    catch (const std::exception& e)
    {
        return jsonResponse(errorEnvelope(
            {}, parseErrorCode, std::string {"Parse error: "} + e.what()));
    }

    if (!message.isObject())
        return jsonResponse(errorEnvelope({},
                                          invalidRequestCode,
                                          "Expected a single JSON-RPC message "
                                          "object (batches are not supported)"));

    auto method = field(message, "method");
    auto id = field(message, "id");

    // No method -> a response from the client; no id -> a notification.
    // Neither produces a JSON-RPC reply.
    if (!method.isString() || id.isNull())
        return acceptedResponse();

    return jsonResponse(
        handleRequest(method.asString(), field(message, "params"), id));
}

Miro::JSON Server::handleRequest(const std::string& method,
                                 const Miro::JSON& params,
                                 const Miro::JSON& id)
{
    if (method == "initialize")
        return resultEnvelope(id, initializeResult(params));

    if (method == "ping")
        return resultEnvelope(id, makeObject());

    if (method == "tools/list")
        return resultEnvelope(id, listToolsResult());

    if (method == "tools/call")
    {
        try
        {
            return resultEnvelope(id, callToolResult(params));
        }
        catch (const std::invalid_argument& e)
        {
            return errorEnvelope(id, invalidParamsCode, e.what());
        }
    }

    return errorEnvelope(id, methodNotFoundCode, "Method not found: " + method);
}

Miro::JSON Server::initializeResult(const Miro::JSON& params) const
{
    // Echo the client's requested version when given — the subset we
    // implement (plain-JSON POST, tools only) is identical across the
    // protocol revisions in the wild.
    auto requested = field(params, "protocolVersion");

    auto capabilities = makeObject();
    capabilities.asObject()["tools"] = makeObject();

    auto serverInfo = makeObject();
    serverInfo.asObject()["name"] = Miro::JSON {name};
    serverInfo.asObject()["version"] = Miro::JSON {version};

    auto result = makeObject();
    result.asObject()["protocolVersion"] =
        requested.isString() ? requested : Miro::JSON {protocolVersion};
    result.asObject()["capabilities"] = capabilities;
    result.asObject()["serverInfo"] = serverInfo;

    if (!instructions.empty())
        result.asObject()["instructions"] = Miro::JSON {instructions};

    return result;
}

Miro::JSON Server::listToolsResult() const
{
    auto list = Miro::Json::Array {};

    for (const auto& tool: tools)
    {
        auto json = makeObject();
        json.asObject()["name"] = Miro::JSON {tool.name};
        json.asObject()["description"] = Miro::JSON {tool.description};
        json.asObject()["inputSchema"] = tool.inputSchema;
        list.add(json);
    }

    auto result = makeObject();
    result.asObject()["tools"] = Miro::JSON {std::move(list)};
    return result;
}

Miro::JSON Server::callToolResult(const Miro::JSON& params)
{
    auto name = field(params, "name");
    if (!name.isString())
        throw std::invalid_argument("tools/call: missing tool 'name'");

    auto arguments = field(params, "arguments");
    if (arguments.isNull())
        arguments = makeObject();

    for (auto& tool: tools)
    {
        if (tool.name != name.asString())
            continue;

        try
        {
            return resultToJson(tool.handler(arguments));
        }
        catch (const std::exception& e)
        {
            return resultToJson(toolError(e.what()));
        }
    }

    throw std::invalid_argument("tools/call: unknown tool: " + name.asString());
}

} // namespace eacp::MCP
