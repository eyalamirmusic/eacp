add_subdirectory(Core)
add_subdirectory(SIMD)
add_subdirectory(Network)

# Always added: the directory builds the shader codegen library
# (eacp-gpu-codegen), which is pure string generation and needs no GPU. It
# returns before eacp-gpu unless EACP_HAS_DRAW.
add_subdirectory(GPU)

# Always added: the directory builds the page bridge (eacp-webview-bridge),
# which speaks to a ScriptHost and needs no native web runtime. It returns
# before eacp-webview unless EACP_HAS_WEBVIEW.
add_subdirectory(WebView)

if (EACP_HAS_DRAW)
    add_subdirectory(Graphics)
endif ()

if (EACP_HAS_GPU)
    # Portable C++ on top of eacp-gpu, which the GPU directory above built.
    add_subdirectory(GPUWidgets)

    # Batched textured quads, also on eacp-gpu alone - no glyphs involved.
    add_subdirectory(Sprites)
endif ()

if (EACP_HAS_TEXT)
    # Glyph rasterization + atlas caching, on top of the GPU module.
    add_subdirectory(Text)

    # Lightweight component tier: a whole widget tree in one GPUView, drawn
    # through the sprite and glyph batchers.
    add_subdirectory(UI)

    # After UI, because a document now builds into either tier: native shape
    # layers, or a component tree drawn through the coverage rasterizer. Only
    # the first of those needs EACP_HAS_CONTEXT, so the module is here with the
    # rest of the text tier and its native builder alone is gated inside.
    add_subdirectory(SVG)
endif ()

if (EACP_HAS_CAPTURE)
    add_subdirectory(Camera)
    add_subdirectory(CameraView)

    # View->video recorder, and video decode/playback. The Snapshot tier is
    # portable (AVFoundation on macOS, Media Foundation on Windows); the Screen
    # tier is ScreenCaptureKit on macOS and a stub on Windows until
    # Windows.Graphics.Capture lands. VideoView draws a decoded stream through
    # the sprite renderer, the way CameraView draws a live camera.
    if (NOT IOS)
        add_subdirectory(Video)
        add_subdirectory(VideoView)
    endif ()
endif ()
