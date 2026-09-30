set(CMAKE_FOLDER Examples)

if (ANDROID)
    add_subdirectory(Android)
    add_subdirectory(GPU/Triangle)
    add_subdirectory(GPU/GlyphAtlas)
    return ()
endif ()

# The web builds the Android example - a GPUView, the shader EDSL and the glyph
# atlas - and the GPU examples, none of which needs the 2D tier or the network;
# Apps/GPU leaves out the ones the web cannot run.
if (EMSCRIPTEN)
    add_subdirectory(Android/HelloGPU)
    add_subdirectory(GPU)
    return ()
endif ()

add_subdirectory(Console)
add_subdirectory(Network)

# Core ML is Apple's; the spike is a macOS console app.
if (EACP_HAS_COREML AND NOT IOS)
    add_subdirectory(ML)
endif ()

if (EACP_HAS_GPU)
    add_subdirectory(GPU)

    # Every host and plugin under here draws a GPUView, so a window and a
    # device are the whole of what the pairs need.
    add_subdirectory(Plugins)
endif ()

# These two paint through the platform's own 2D stack.
if (EACP_HAS_CONTEXT)
    add_subdirectory(Graphics)
    add_subdirectory(SVG)
endif ()

# The component tier draws itself in one GPUView through the glyph and sprite
# batchers, so a window and a rasterizer are the whole of what it needs.
if (EACP_HAS_TEXT)
    add_subdirectory(UI)

    # Video decode + playback through the GPU display stack. No WebView needed.
    if (EACP_HAS_CAPTURE AND NOT IOS)
        add_subdirectory(Video)
    endif ()

    if (EACP_HAS_WEBVIEW)
        add_subdirectory(WebView)
        add_subdirectory(Camera)
        add_subdirectory(Mixed)
    endif ()
endif ()
