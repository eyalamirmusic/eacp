# Client-side library for driving WebView apps from C++ tests over
# the embedded test RPC server. The server lives in ../WebView/
# (TestServer.cpp); this is the matching client.
#
# Builds on Apple-not-iOS and Windows — the platforms where launching
# a child WebView app makes sense. On iOS / Linux / mobile, e2e tests
# would need a different transport (the WebView itself isn't a desktop
# windowed app), so we don't ship this lib there.
if (NOT ((APPLE AND NOT IOS) OR WIN32))
    return ()
endif ()

add_library(eacp-webview-test STATIC)

target_sources(eacp-webview-test PRIVATE
        AppDriver.cpp
        Launch.cpp)

if (APPLE)
    target_sources(eacp-webview-test PRIVATE Process-Apple.cpp)
elseif (WIN32)
    target_sources(eacp-webview-test PRIVATE Process-Windows.cpp)
endif ()

# eacp-network-rpc pulls in eacp-network (HTTP client) + Miro (JSON)
# + eacp-core (include path for <eacp/...>).
target_link_libraries(eacp-webview-test PUBLIC eacp-network-rpc)

set_default_target_setting(eacp-webview-test)
eacp_enable_unity_build(eacp-webview-test)
add_ide_sources(eacp-webview-test)
