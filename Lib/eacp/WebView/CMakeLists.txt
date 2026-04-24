add_library(eacp-webview STATIC)

set_default_target_setting(eacp-webview)
add_ide_sources(eacp-webview)

target_link_libraries(eacp-webview PUBLIC eacp-graphics)

target_sources(eacp-webview PRIVATE WebView.cpp)

if (APPLE)
    target_sources(eacp-webview PRIVATE WebView.mm)
    target_link_libraries(eacp-webview PRIVATE
            "-framework Foundation"
            "-framework WebKit")

    if (EACP_IOS)
        target_link_libraries(eacp-webview PRIVATE
                "-framework UIKit")
    else ()
        target_link_libraries(eacp-webview PRIVATE
                "-framework AppKit")
    endif ()
elseif (WIN32)
    target_sources(eacp-webview PRIVATE WebView-Windows.cpp)

    find_package(WebView2 REQUIRED)

    target_link_libraries(eacp-webview PRIVATE
            WebView2::WebView2)
endif ()
