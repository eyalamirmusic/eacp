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

    add_executable(WebViewAffordanceTests WebViewAffordanceTests.cpp)
    target_link_libraries(WebViewAffordanceTests PRIVATE
            eacp-webview
            eacp-webview-test-main)
    nano_discover_tests(WebViewAffordanceTests)

    # Drives View::renderToImageAsync on a real WebView: the async snapshot folds
    # the page's pixels into the composited image, awaited with waitFor. The
    # snapshot backend is Apple-only for now (Windows renderToImage/Async are
    # stubbed to an invalid Image -- see View-Windows.cpp), so gate the pixel
    # assertions on APPLE until the Windows off-screen target is implemented.
    if (APPLE)
        add_executable(RenderToImageAsyncTests RenderToImageAsyncTests.cpp)
        target_link_libraries(RenderToImageAsyncTests PRIVATE
                eacp-webview
                eacp-webview-test-main)
        nano_discover_tests(RenderToImageAsyncTests)
    endif ()

    # Drives the unhandled-key forwarding pipeline (key-events.js + the native
    # verdict queue) on a real WKWebView. Synthesizing the key NSEvents is
    # AppKit-specific, so macOS only.
    if (APPLE)
        add_executable(KeyForwardingTests KeyForwardingTests.mm)
        target_link_libraries(KeyForwardingTests PRIVATE
                eacp-webview
                eacp-webview-test-main)
        nano_discover_tests(KeyForwardingTests)
    endif ()

    # Windows counterpart: the WebView2 backend keeps no native event stash, so
    # the verdict message carries the key identity and DOM-dispatched events
    # drive the whole path without OS key injection (see the file header).
    if (WIN32)
        add_executable(KeyForwardingTestsWindows KeyForwardingTests-Windows.cpp)
        target_link_libraries(KeyForwardingTestsWindows PRIVATE
                eacp-webview
                eacp-webview-test-main)
        nano_discover_tests(KeyForwardingTestsWindows)
    endif ()
endif ()
