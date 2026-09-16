set(CMAKE_FOLDER Examples)

add_subdirectory(Console)
add_subdirectory(Network)

if (EACP_HAS_GPU)
    add_subdirectory(GPU)
endif ()

# These two paint through the platform's own 2D stack.
if (EACP_HAS_CONTEXT)
    add_subdirectory(Graphics)
    add_subdirectory(SVG)
endif ()

# Half of this one does too, and says so itself: the X11 host and plugin below
# it need only a window and the GPU, so Linux enters it as well.
if (EACP_HAS_CONTEXT OR LINUX)
    add_subdirectory(Plugins)
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
