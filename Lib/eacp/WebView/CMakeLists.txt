include(FetchContent)

find_package(ResEmbed REQUIRED)
find_package(Miro REQUIRED)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/EacpWebViewVite.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/EacpWebViewBackend.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/EacpWebViewReact.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/EacpWebViewApp.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/EacpWebViewNodeTests.cmake)

add_library(eacp-webview STATIC)

target_link_libraries(eacp-webview PUBLIC eacp-graphics ResEmbed Miro)

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
        WebView/MenuHelpers.cpp)

if (APPLE)
    target_sources(eacp-webview PRIVATE
            WebView/WebView.mm
            WebView/DevServerProbe-Posix.cpp)
    target_link_libraries(eacp-webview PRIVATE
            "-framework Foundation"
            "-framework WebKit")

    if (IOS)
        target_link_libraries(eacp-webview PRIVATE
                "-framework UIKit")
    else ()
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

# --- Agentic-QA debug control (opt-in) ----------------------------------
# Bakes the window.eacpQA helper JS into a header so DebugControl can inject
# it with no runtime file dependency. Re-run cmake after editing eacp-qa.js.
set(_qa_js ${CMAKE_CURRENT_LIST_DIR}/Debug/eacp-qa.js)
set(_qa_header ${CMAKE_CURRENT_BINARY_DIR}/EacpQAScript.generated.h)
file(READ ${_qa_js} _qa_js_contents)
file(WRITE ${_qa_header}
        "#pragma once\n"
        "namespace eacp::Graphics\n{\n"
        "inline constexpr const char* kEacpQAScript = R\"EACPQA(\n"
        "${_qa_js_contents}"
        ")EACPQA\";\n"
        "} // namespace eacp::Graphics\n")

add_library(eacp-webview-debug STATIC Debug/DebugControl.cpp)
target_include_directories(eacp-webview-debug PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(eacp-webview-debug PUBLIC eacp-webview eacp-network)
set_default_target_setting(eacp-webview-debug)
eacp_enable_unity_build(eacp-webview-debug)
add_ide_sources(eacp-webview-debug)

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

add_subdirectory(Test)
