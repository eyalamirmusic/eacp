set(CMAKE_FOLDER Examples)

add_subdirectory(Console)
add_subdirectory(Network)

if (EACP_HAS_GPU)
    add_subdirectory(GPU)
endif ()

# Every example in these three paints through the platform's own 2D stack: the
# Graphics ones fill a Graphics::Context and hang menus and a tray off the app,
# the plugin ones host a ShapeLayerView, and the SVG one calls SVG::parse for
# the native layer tier. None of that exists on Linux, hence EACP_HAS_CONTEXT
# rather than EACP_HAS_TEXT.
if (EACP_HAS_CONTEXT)
    add_subdirectory(Plugins)
    add_subdirectory(Graphics)
    add_subdirectory(SVG)
endif ()

# The component tier draws itself in one GPUView through the glyph and sprite
# batchers, so a window and a rasterizer are the whole of what it needs.
if (EACP_HAS_TEXT)
    add_subdirectory(UI)

    # Video decode + playback through the GPU display stack. No WebView needed.
    # Capture has no Linux backend, so this is off there however far the
    # drawing stack gets.
    if (EACP_HAS_CAPTURE AND NOT IOS)
        add_subdirectory(Video)
    endif ()

    # Each of these hosts a native web view beside the rest of the UI, so they
    # need a runtime to wrap as well as the text tier they are written in -
    # and there is none on Linux either.
    if (EACP_HAS_WEBVIEW)
        add_subdirectory(WebView)
        add_subdirectory(Camera)
        add_subdirectory(Mixed)
    endif ()
endif ()
