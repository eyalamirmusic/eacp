nano_add_executable(StreamingRangeTests
        SOURCES StreamingRangeTests.cpp
        TARGETS eacp-webview)

nano_add_executable(FileProviderTests
        SOURCES FileProviderTests.cpp
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
endif ()
