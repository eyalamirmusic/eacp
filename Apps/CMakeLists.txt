set(CMAKE_FOLDER Examples)

add_subdirectory(Console)
add_subdirectory(Network)

if (EACP_HAS_GPU)
    add_subdirectory(GPU)
endif ()

# Every example in these four draws text or a Path, so they need the whole 2D
# and glyph stack rather than only a window - which is why none of them is in
# the Linux build while EACP_HAS_TEXT is off there.
if (EACP_HAS_TEXT)
    add_subdirectory(Plugins)
    add_subdirectory(Graphics)
    add_subdirectory(SVG)
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
