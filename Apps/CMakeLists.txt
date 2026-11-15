set(CMAKE_FOLDER Examples)

if (ANDROID)
    add_subdirectory(Android)
    add_subdirectory(GPU)
    add_subdirectory(UI)
    add_subdirectory(SVG)
    add_subdirectory(Camera)
    return ()
endif ()

add_subdirectory(Console)
add_subdirectory(Network)

# The examples written for the browser's loop, beside the ones above that run
# there unchanged.
if (EMSCRIPTEN)
    add_subdirectory(Web)
endif ()

# Core ML is Apple's; the spike is a macOS console app.
if (EACP_HAS_COREML AND NOT IOS)
    add_subdirectory(ML)
endif ()

if (EACP_HAS_GPU)
    add_subdirectory(GPU)

    # Every host and plugin under here draws a GPUView, so a window and a
    # device are the whole of what the pairs need, but each pair loads a
    # dynamic library, which the web cannot.
    if (NOT EMSCRIPTEN)
        add_subdirectory(Plugins)
    endif ()
endif ()

# Desktop-shell examples: a few paint through the platform's own 2D stack, the
# rest are the component tier, each gated on its own needs inside. Linux has
# the component tier, but none of these has been built there yet.
if (EACP_HAS_CONTEXT OR (EACP_HAS_TEXT AND NOT LINUX))
    add_subdirectory(Graphics)
endif ()

# The component tier draws itself in one GPUView through the glyph and sprite
# batchers, so a window and a rasterizer are the whole of what it needs.
if (EACP_HAS_TEXT)
    add_subdirectory(UI)
    add_subdirectory(SVG)

    # Video decode + playback through the GPU display stack. No WebView needed.
    if (EACP_HAS_CAPTURE AND NOT IOS)
        add_subdirectory(Video)
    endif ()

    if (EACP_HAS_WEBVIEW)
        add_subdirectory(WebView)
        add_subdirectory(Mixed)
    endif ()
endif ()

if (EACP_HAS_CAPTURE)
    add_subdirectory(Camera)
endif ()
