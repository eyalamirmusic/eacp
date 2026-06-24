# Window debug/capture server: an MCP server bound to a Window that
# exposes screenshot + screen recording over HTTP, for ANY eacp app —
# GPU, native drawing, SVG, WebView. It depends only on graphics +
# networking, never on the WebView, so capture is a window-level
# affordance rather than a WebView feature. Higher layers (the WebView
# DOM tools) enrich the same server via a ServerExtension.
add_library(eacp-graphics-remote STATIC)

# WindowAutoAttachRegister.cpp is deliberately absent: the debug-server
# cmake helper compiles it straight into debug-enabled app binaries so
# its static initializer (which installs the Window hook) can't be
# dead-stripped as an unreferenced archive member.
target_sources(eacp-graphics-remote PRIVATE
        WindowAutoAttach.cpp
        WindowDebugServer.cpp)

target_link_libraries(eacp-graphics-remote PUBLIC
        eacp-graphics
        eacp-network-mcp)

set_default_target_setting(eacp-graphics-remote)
eacp_enable_unity_build(eacp-graphics-remote)
add_ide_sources(eacp-graphics-remote)
