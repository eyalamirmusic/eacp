# WebView debug tools: the AppDriver's DOM-driving capabilities as MCP
# tools, layered onto the window-level capture server (eacp-graphics-
# remote) so a WebView app exposes one endpoint with capture + DOM.
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
        WebViewTools.cpp)

target_link_libraries(eacp-webview-remote PUBLIC
        eacp-webview-test
        eacp-graphics-remote
        eacp-network-mcp)

# Console/error capture script, injected into every page the debug
# tools attach to.
res_embed_add(eacp-webview-remote
        FILES     Resources/console-capture.js
        NAMESPACE RemoteAgent
        CATEGORY  RemoteAgent)

set_default_target_setting(eacp-webview-remote)
eacp_enable_unity_build(eacp-webview-remote)
add_ide_sources(eacp-webview-remote)
