nano_add_executable(StreamingRangeTests
        SOURCES StreamingRangeTests.cpp
        TARGETS eacp-webview)

nano_add_executable(FileProviderTests
        SOURCES FileProviderTests.cpp
        TARGETS eacp-webview)

nano_add_executable(AsyncBridgeTests
        SOURCES AsyncBridgeTests.cpp
        TARGETS eacp-webview)

# Drives the live streaming pump on a real WebView, so it needs the
# NSApplication / runloop bootstrap from eacp-webview-test-main rather than
# nano_add_executable's bare main(). That target only exists on the platforms
# where the in-process WebView test harness is built (Apple-not-iOS, Windows).
if (TARGET eacp-webview-test-main)
    add_executable(StreamingPumpTests StreamingPumpTests.cpp)
    target_link_libraries(StreamingPumpTests PRIVATE
            eacp-webview
            eacp-webview-test-main)
    nano_discover_tests(StreamingPumpTests)

    # Drives the C++ -> page call path on a real WebView, so it needs the
    # same NSApplication / runloop bootstrap as StreamingPumpTests.
    add_executable(WebViewCallTests WebViewCallTests.cpp)
    target_link_libraries(WebViewCallTests PRIVATE
            eacp-webview
            eacp-webview-test-main)
    nano_discover_tests(WebViewCallTests)

    # Drives page -> C++ command dispatch on a real WebView: per-command
    # execution mode (main vs worker thread) and async Completer handlers.
    add_executable(CommandDispatchTests CommandDispatchTests.cpp)
    target_link_libraries(CommandDispatchTests PRIVATE
            eacp-webview
            eacp-webview-test-main)
    nano_discover_tests(CommandDispatchTests)

    # window-drag.js is injected by both desktop backends (see
    # installWindowDragSupport in WebView-Shared.cpp), so the classification
    # tests run wherever the test harness exists. WebKit-specific assertions
    # guard themselves per-platform inside the file.
    add_executable(WindowDragTests WindowDragTests.cpp)
    target_link_libraries(WindowDragTests PRIVATE
            eacp-webview
            eacp-webview-test-main)
    nano_discover_tests(WindowDragTests)

    # Same deal for window-controls.js (see installWindowControlSupport):
    # caption-button classification and the <html> attributes the shim keeps.
    add_executable(WindowControlTests WindowControlTests.cpp)
    target_link_libraries(WindowControlTests PRIVATE
            eacp-webview
            eacp-webview-test-main)
    nano_discover_tests(WindowControlTests)
endif ()
