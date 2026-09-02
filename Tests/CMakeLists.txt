set(CMAKE_FOLDER Tests)

find_package(NanoTest REQUIRED)

add_subdirectory(Core)
add_subdirectory(SIMD)

if (APPLE)
    add_subdirectory(ObjC)
endif ()

add_subdirectory(Network)

if ((APPLE OR WIN32) AND EACP_BUILD_GRAPHICS)
    add_subdirectory(Graphics)
endif ()

if ((APPLE OR WIN32) AND EACP_BUILD_GRAPHICS AND NOT IOS)
    add_subdirectory(GPU)
    add_subdirectory(Text)
    add_subdirectory(GPUWidgets)
    add_subdirectory(UI)

    # The parse layer and the geometry it builds. Above UI in the list because
    # SVGComponent is a UI::Component, and none of these needs a window.
    add_subdirectory(SVG)
    add_subdirectory(Camera)

    # The video playback stack above the Decoder interface — frame queueing,
    # playhead-to-frame selection, the clock — is portable C++, so these run
    # against a synthetic decoder with no media file and no codec involved.
    add_subdirectory(Video)
endif ()

# Codegen tests exercise eacp's WebView events/hooks format registrations,
# so they only run on the platforms where the WebView library (and its
# eacp-webview-codegen object lib) are built.
if ((APPLE OR WIN32) AND EACP_BUILD_GRAPHICS AND EACP_BUILD_WEBVIEW)
    add_subdirectory(Codegen)
endif ()

# Pure-logic WebView tests (Range header resolution), plus the bridge over a
# ScriptHost that is no WebView -- which is why the gate stops at the graphics
# one: eacp-webview-bridge builds without EACP_BUILD_WEBVIEW, and the module
# skips the suites that need a real WebView when it is off.
if ((APPLE OR WIN32) AND EACP_BUILD_GRAPHICS)
    add_subdirectory(WebView)
endif ()

