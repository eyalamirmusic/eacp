#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Network/HTTPRpc/RpcServer.h>
#include <eacp/Network/HTTPServer/HttpServer.h>
#include <eacp/Network/MCP/McpServer.h>
#include <eacp/WebView/Test/AppDriver.h>

#include <Miro/Miro.h>

#include <functional>
#include <string>

namespace eacp::WebView::Remote
{

struct DebugServerOptions
{
    // Port to listen on; 0 binds an ephemeral port — read the actual
    // one back via port().
    int port = 0;

    // Directory the snapshot tool writes into. Empty -> AppDriver's
    // default (<cwd>/test-results/snapshots).
    std::string snapshotDir;
};

// Embeds an MCP server into a running app so an external agent can
// drive and debug the live WebView over HTTP: list elements tagged
// with the ElementIds attribute, click / fill / press, evaluate JS,
// capture screenshots, read console logs, and invoke bridge commands
// directly. Attach an MCP client to it:
//
//     claude mcp add --transport http myapp http://localhost:<port>/mcp
//
// The bridge is also mounted as a plain RPC endpoint at /rpc (see
// HTTP::Rpc::Server) for curl-style access without MCP framing.
//
// Apps normally don't construct this directly: eacp_add_webview_app
// compiles the auto-attach hook into the binary when EACP_DEBUG_SERVER
// allows it (AUTO -> Debug builds only) and every WebViewBridge then
// attaches a server by itself — see AutoAttach.h for the port policy
// (EACP_DEBUG_PORT). Manual construction works too, at startup or
// later; attaching to an already-loaded page is supported. Tool calls
// are dispatched onto the main thread and pump the event loop while
// they run (same model as AppDriver), so the app keeps animating
// during a call.
//
// This is a development/debugging tool: it accepts any connection the
// OS routes to the port and executes arbitrary JS in the page. Release
// builds exclude it unless EACP_DEBUG_SERVER=ON is forced at configure
// time — don't ship it enabled.
class DebugServer
{
public:
    DebugServer(Graphics::WebView& webViewToUse,
                Miro::Bridge& bridgeToUse,
                DebugServerOptions options = {});
    ~DebugServer();

    DebugServer(const DebugServer&) = delete;
    DebugServer& operator=(const DebugServer&) = delete;

    int port() const;

    // The transport entry point, exposed for tests and custom mounts.
    HTTP::Response handleMcp(const HTTP::Request& request);

private:
    void installConsoleCapture();
    void registerTools();
    void addTool(std::string name,
                 std::string description,
                 const std::string& schemaJson,
                 MCP::ToolHandler handler);

    Graphics::WebView& webView;
    Test::AppDriver driver;
    MCP::Server mcp;
    HTTP::Server http;
    OwningPointer<HTTP::Rpc::Server> rpc;
    std::function<void(const std::string&)> previousFinishedHandler;
    bool busy = false;
};

} // namespace eacp::WebView::Remote
