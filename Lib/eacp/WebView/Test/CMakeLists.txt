# Client-side library for driving WebView apps from C++ tests
# in-process. The driver runs on the main thread alongside the
# WebView, pumping runEventLoopFor() between operations so
# evaluateJavaScript/takeSnapshot callbacks fire.
#
# Builds on Apple-not-iOS and Windows — the platforms where the
# WebView itself runs as a desktop windowed app. On iOS / Linux we
# don't ship this lib.
if (NOT ((APPLE AND NOT IOS) OR WIN32))
    return ()
endif ()

add_library(eacp-webview-test STATIC)

target_sources(eacp-webview-test PRIVATE
        AppDriver.cpp
        DomNode.cpp
        TestAgent.cpp)

# eacp-webview gives us WebView + WebViewBridge + Miro::Bridge.
target_link_libraries(eacp-webview-test PUBLIC eacp-webview eacp-network-rpc)

# Embed the JS test agent so loadTestAgentSource() can pull it out
# at runtime. Lives at Resources/test-agent.js; consumers don't see
# the file path — they just get the source string.
res_embed_add(eacp-webview-test
        FILES     Resources/test-agent.js
        NAMESPACE TestAgent
        CATEGORY  TestAgent)

set_default_target_setting(eacp-webview-test)
eacp_enable_unity_build(eacp-webview-test)
add_ide_sources(eacp-webview-test)

# Prebuilt main() for test binaries — runs nano::run() from inside
# Apps::run<TestRunner>, so the framework's standard runloop setup
# is in place before any test constructs a Window/WebView. NanoTest
# itself is pulled in lazily because the rest of eacp doesn't need
# it.
find_package(NanoTest REQUIRED)

add_library(eacp-webview-test-main STATIC TestMain.cpp)
if (WIN32)
    target_sources(eacp-webview-test-main PRIVATE TestCrashGuard-Windows.cpp)
else ()
    target_sources(eacp-webview-test-main PRIVATE TestCrashGuard-Default.cpp)
endif ()
target_link_libraries(eacp-webview-test-main PUBLIC NanoTest eacp-core)
set_default_target_setting(eacp-webview-test-main)
