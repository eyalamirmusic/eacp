include(FetchContent)

find_package(ResEmbed REQUIRED)
find_package(Miro REQUIRED)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/EacpWebViewVite.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/EacpWebViewBackend.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/EacpWebViewReact.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/EacpWebViewApp.cmake)

# Compiles a small HTTP RPC server into WebViewBridge that injects a
# DOM-driving JS agent and exposes it as `test.*` commands. An
# external Node runner can POST to /rpc to drive any WebView app.
# The flag is read at runtime by TestServer::shouldEnableTestServer()
# so the surrounding code stays free of preprocessor branches —
# enabling/disabling only flips whether the server is instantiated,
# not whether the code compiles.
option(EACP_WEBVIEW_ENABLE_TEST_SERVER
        "Instantiate the embedded WebView test/RPC server at runtime" ON)

add_library(eacp-webview STATIC)

target_link_libraries(eacp-webview PUBLIC eacp-graphics ResEmbed Miro)

# eacp-network-rpc pulls in eacp-network for HTTP::Server. Linked
# unconditionally — the test server code is always compiled in; the
# option above only gates whether the server runs.
target_link_libraries(eacp-webview PRIVATE eacp-network-rpc)

target_compile_definitions(eacp-webview PUBLIC
        EACP_WEBVIEW_ENABLE_TEST_SERVER=$<BOOL:${EACP_WEBVIEW_ENABLE_TEST_SERVER}>)

# WKUIDelegate's public camera/mic permission selector is macOS 12+. When the
# macOS deployment target is older, opt into WebKit's private _WK SPI so
# getUserMedia still works on macOS 11. iOS uses a different SPI we don't
# implement here, so this is macOS-only.
if (APPLE AND NOT IOS AND CMAKE_OSX_DEPLOYMENT_TARGET
        AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "12.0")
    target_compile_definitions(eacp-webview PRIVATE
            EACP_WEBVIEW_PRIVATE_MEDIA_CAPTURE_SPI=1)
endif ()

target_sources(eacp-webview PRIVATE
        WebView/WebView-Shared.cpp
        WebView/DevServerProbe.cpp
        WebView/Bridge.cpp
        WebView/StateBridge.cpp
        WebView/MenuHelpers.cpp
        WebView/TestServer.cpp)

# Embed the JS test agent so TestServer.cpp can pull it out at
# runtime via ResEmbed::get("test-agent.js", "EacpTestServer").
res_embed_add(eacp-webview
        FILES     WebView/test-agent.js
        NAMESPACE EacpTestServer
        CATEGORY  EacpTestServer)

if (APPLE)
    target_sources(eacp-webview PRIVATE
            WebView/WebView.mm
            WebView/DevServerProbe-Posix.cpp)
    target_link_libraries(eacp-webview PRIVATE
            "-framework Foundation"
            "-framework WebKit")

    if (IOS)
        target_sources(eacp-webview PRIVATE
                WebView/Snapshot-iOS.mm
                WebView/WebViewPlatform-iOS.mm)
        target_link_libraries(eacp-webview PRIVATE
                "-framework UIKit")
    else ()
        target_sources(eacp-webview PRIVATE
                WebView/Snapshot-macOS.mm
                WebView/WebViewPlatform-macOS.mm)
        target_link_libraries(eacp-webview PRIVATE
                "-framework AppKit")
    endif ()
elseif (WIN32)
    target_sources(eacp-webview PRIVATE
            WebView/WebView-Windows.cpp
            WebView/DevServerProbe-Windows.cpp)

    find_package(WebView2 REQUIRED)

    target_link_libraries(eacp-webview PRIVATE
            WebView2::WebView2
            ws2_32)
else ()
    target_sources(eacp-webview PRIVATE WebView/DevServerProbe-Posix.cpp)
endif ()

set_default_target_setting(eacp-webview)
eacp_enable_unity_build(eacp-webview)
add_ide_sources(eacp-webview)

# Codegen-only object library — bundles the eacp events / hooks
# formatters and their static-init registration with Miro's format
# registry. Spliced into each app's ${TARGET}Schema_Codegen executable
# by eacp_add_webview_app, so the format registrations only run inside
# the build-time codegen tool, never in the shipping runtime.
#
# Skipped when cross-compiling — symmetric with MiroTypeExportMain.
if (NOT CMAKE_CROSSCOMPILING)
    add_library(eacp-webview-codegen OBJECT
            Codegen/EventsFormat.cpp
            Codegen/HooksFormat.cpp
            Codegen/Register.cpp)
    target_link_libraries(eacp-webview-codegen PUBLIC Miro)
    set_default_target_setting(eacp-webview-codegen)
endif ()

# Client-side test driver — sibling of TestServer.cpp. Self-gates to
# Apple-not-iOS + Windows; iOS / Linux are no-ops.
add_subdirectory(Test)
