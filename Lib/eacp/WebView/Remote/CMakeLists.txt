# Remote debug server: mounts the AppDriver capabilities as MCP tools
# over HTTP so an external agent can drive and debug a live app.
# Reuses the in-process driver, so it shares its platform gate —
# desktop (Apple-not-iOS + Windows) only.
if (NOT TARGET eacp-webview-test)
    return ()
endif ()

add_library(eacp-webview-remote STATIC)

# AutoAttachRegister.cpp is deliberately absent: eacp_add_webview_app
# compiles it straight into debug-enabled app binaries so its static
# initializer (which installs the WebViewBridge hook) can't be
# dead-stripped as an unreferenced archive member.
target_sources(eacp-webview-remote PRIVATE
        AutoAttach.cpp
        DebugServer.cpp)

target_link_libraries(eacp-webview-remote PUBLIC
        eacp-webview-test
        eacp-network-mcp)

# Console/error capture script, injected into every page the debug
# server attaches to.
res_embed_add(eacp-webview-remote
        FILES     Resources/console-capture.js
        NAMESPACE RemoteAgent
        CATEGORY  RemoteAgent)

set_default_target_setting(eacp-webview-remote)
eacp_enable_unity_build(eacp-webview-remote)
add_ide_sources(eacp-webview-remote)
