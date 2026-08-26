# Pure-logic suites: Range header resolution, the file provider and the async
# bridge. They link eacp-webview but never construct a WebView, so they build
# wherever the library does.
set(webview_test_sources
        AsyncBridgeTests.cpp
        FileProviderTests.cpp
        StreamingRangeTests.cpp)

# The drag-out drop source/target that stream onFileDragMoved / onFileDragEnded
# through SHDoDragDrop's modal loop (FileDrag-Windows.h). The cursor reader is
# injected, so the COM verdict logic and the physical->CSS pixel mapping test
# without a live drag or window.
if (WIN32)
    list(APPEND webview_test_sources FileDragTests-Windows.cpp)
endif ()

# The rest drive a real WebView, so the whole module needs the NSApplication /
# runloop bootstrap from eacp-webview-test-main rather than nano_add_executable's
# bare main(). That target only exists on the platforms where the in-process
# WebView test harness is built (Apple-not-iOS, Windows); elsewhere the module
# is just the pure-logic suites above.
#
#   StreamingPumpTests      the live streaming pump
#   WebViewCallTests        the C++ -> page call path
#   CommandDispatchTests    page -> C++ dispatch: per-command execution mode
#                           (main vs worker thread) and async Completer handlers
#   WindowDragTests         window-drag.js classification (installWindowDragSupport
#                           in WebView-Shared.cpp injects it on both desktop
#                           backends; WebKit-specific assertions guard themselves
#                           per-platform inside the file)
#   WindowControlTests      same deal for window-controls.js (see
#                           installWindowControlSupport): caption-button
#                           classification and the <html> attributes the shim keeps
#   RenderToImageAsyncTests View::renderToImageAsync folding the page's pixels
#                           into the composited image, awaited with waitFor
#   OffscreenAnimationTests Options::driveOffscreenAnimation keeping an rAF loop
#                           advancing off-screen, compared as captured pixels
#   UserAgentTests          Options::userAgent read back from the page, and
#                           Options::loadDeclinedPopupsInline deciding whether a
#                           declined window.open replaces the host document
if (TARGET eacp-webview-test-main)
    list(APPEND webview_test_sources
            CommandDispatchTests.cpp
            OffscreenAnimationTests.cpp
            RenderToImageAsyncTests.cpp
            StreamingPumpTests.cpp
            UserAgentTests.cpp
            WebViewAffordanceTests.cpp
            WebViewCallTests.cpp
            WindowControlTests.cpp
            WindowDragTests.cpp)

    # Unhandled-key forwarding (key-events.js + the native verdict queue).
    # Synthesizing the key NSEvents is AppKit-specific; the WebView2 backend
    # keeps no native event stash, so its counterpart carries the key identity
    # in the verdict message and drives the path with DOM-dispatched events
    # instead of OS key injection (see the file headers).
    if (APPLE)
        list(APPEND webview_test_sources KeyForwardingTests.mm)
    endif ()

    if (WIN32)
        list(APPEND webview_test_sources KeyForwardingTests-Windows.cpp)
    endif ()

    add_executable(WebViewTests ${webview_test_sources})
    target_link_libraries(WebViewTests PRIVATE
            eacp-webview
            eacp-webview-test-main)
    nano_discover_tests(WebViewTests)
else ()
    nano_add_executable(WebViewTests
            SOURCES ${webview_test_sources}
            TARGETS eacp-webview)
endif ()
