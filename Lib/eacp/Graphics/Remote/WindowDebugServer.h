#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Graphics/Helpers/ScreenRecorder.h>
#include <eacp/Network/HTTPServer/HttpServer.h>
#include <eacp/Network/MCP/McpServer.h>

#include <string>

namespace eacp::Graphics
{
class Window;
}

namespace eacp::Graphics::Remote
{

struct WindowDebugServerOptions
{
    // Port to listen on; 0 binds an ephemeral port — read it back via
    // port().
    int port = 0;

    // Directory recordings are written into. Empty -> <cwd>/test-results/
    // recordings.
    std::string recordingDir;
};

class WindowDebugServer;

// A capability a higher layer bolts onto the window debug server (the
// WebView DOM tools, in practice). The server adopts it, calls
// registerTools() once, and owns it for its lifetime — so the WebView
// layer can enrich the server without this lib depending on the WebView.
struct ServerExtension
{
    virtual ~ServerExtension() = default;
    virtual void registerTools(WindowDebugServer& server) = 0;
};

// An MCP server bound to a single Window, exposing the window-level debug
// capabilities every eacp app can offer regardless of what it draws with:
// a screenshot of the composited window and screen recording to MP4. It
// depends only on graphics + networking, so GPU/native/SVG apps get it too.
//
// Apps don't construct this — the window auto-attach hook (WindowAutoAttach.h)
// creates one for the primary window when the build enables the debug server.
// Higher layers add tools via addExtension() (the WebView layer adds DOM
// driving), so one app exposes one endpoint with every capability it linked.
//
// A dev tool: it accepts any connection on the port; release builds exclude it.
class WindowDebugServer
{
public:
    WindowDebugServer(Graphics::Window& windowToUse,
                      WindowDebugServerOptions options = {});
    ~WindowDebugServer();

    WindowDebugServer(const WindowDebugServer&) = delete;
    WindowDebugServer& operator=(const WindowDebugServer&) = delete;

    int port() const;
    Graphics::Window& window();

    // The MCP server, for an extension that needs more than addTool()
    // (e.g. to set richer instructions).
    MCP::Server& mcp();

    // Register an MCP tool whose handler is wrapped in the single-flight
    // guard: a tool call pumps a nested event loop, so a second call
    // arriving mid-pump is refused rather than allowed to re-enter.
    // Built-in tools and extensions both go through this.
    void addTool(std::string name,
                 std::string description,
                 const std::string& schemaJson,
                 MCP::ToolHandler handler);

    // Adopt a capability that registers extra tools onto this server (see
    // ServerExtension). The server keeps it alive for its own lifetime.
    void addExtension(OwningPointer<ServerExtension> extension);

    // The transport entry point, exposed for tests and custom mounts.
    HTTP::Response handleMcp(const HTTP::Request& request);

private:
    void registerWindowTools();
    void registerInputTools();

    Graphics::Window& windowRef;
    MCP::Server mcpServer;
    HTTP::Server http;
    ScreenRecorder recorder;
    std::string recordingDir;
    Vector<OwningPointer<ServerExtension>> extensions;
    bool busy = false;
};

} // namespace eacp::Graphics::Remote
