add_subdirectory(Core)
add_subdirectory(SIMD)
add_subdirectory(Network)

if ((APPLE OR WIN32) AND EACP_BUILD_GRAPHICS)
    add_subdirectory(Graphics)

    # Always added: the directory builds the page bridge (eacp-webview-bridge),
    # which speaks to a ScriptHost and needs no native web runtime.
    # EACP_BUILD_WEBVIEW gates only the WKWebView / WebView2 half inside it.
    add_subdirectory(WebView)
    # The whole GPU stack (Metal) now builds on macOS, iOS and Windows. Sprites,
    # GPUWidgets and CameraView are portable C++ on top of it.
    add_subdirectory(Camera)
    add_subdirectory(GPU)
    add_subdirectory(GPUWidgets)

    # Glyph rasterization + atlas caching, on top of the GPU module.
    add_subdirectory(Text)
    add_subdirectory(Sprites)

    # Lightweight component tier: a whole widget tree in one GPUView, drawn
    # through the sprite and glyph batchers.
    add_subdirectory(UI)

    # After UI, because a document now builds into either tier: native shape
    # layers, or a component tree drawn through the coverage rasterizer.
    add_subdirectory(SVG)

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