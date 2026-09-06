set(CMAKE_FOLDER Tests)

find_package(NanoTest REQUIRED)

add_subdirectory(Core)
add_subdirectory(SIMD)

if (APPLE)
    add_subdirectory(ObjC)
endif ()

add_subdirectory(Network)
add_subdirectory(Spirv)

if (EACP_HAS_DRAW)
    add_subdirectory(Graphics)
endif ()

# Always added, because the shader codegen tests are pure string generation and
# run anywhere. The directory builds those first and returns before the device
# suites unless EACP_HAS_GPU. iOS is excluded whole: it runs no test binaries.
if (NOT IOS)
    add_subdirectory(GPU)
endif ()

if (EACP_HAS_GPU AND NOT IOS)
    add_subdirectory(GPUWidgets)
endif ()

if (EACP_HAS_TEXT AND NOT IOS)
    add_subdirectory(Text)
    add_subdirectory(UI)

    # The parse layer and the geometry it builds. Above UI in the list because
    # SVGComponent is a UI::Component, and none of these needs a window.
    add_subdirectory(SVG)
endif ()

if (EACP_HAS_CAPTURE AND NOT IOS)
    add_subdirectory(Camera)

    # The video playback stack above the Decoder interface — frame queueing,
    # playhead-to-frame selection, the clock — is portable C++, so these run
    # against a synthetic decoder with no media file and no codec involved.
    add_subdirectory(Video)
endif ()

# Codegen tests exercise eacp's WebView events/hooks format registrations,
# so they only run on the platforms where the WebView library (and its
# eacp-webview-codegen object lib) are built.
if (EACP_HAS_WEBVIEW)
    add_subdirectory(Codegen)
endif ()

# Always added: the bridge over a ScriptHost that is no WebView needs no native
# web runtime and no graphics, so ScriptHostTests builds and runs everywhere.
# The directory returns after it unless EACP_HAS_WEBVIEW.
add_subdirectory(WebView)
