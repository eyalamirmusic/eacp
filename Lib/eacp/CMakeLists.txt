add_subdirectory(Core)
add_subdirectory(SIMD)
add_subdirectory(Network)

if ((APPLE OR WIN32) AND EACP_BUILD_GRAPHICS)
    add_subdirectory(Graphics)
    add_subdirectory(SVG)
    if (EACP_BUILD_WEBVIEW)
        add_subdirectory(WebView)
    endif ()
    # The whole GPU stack (Metal) now builds on macOS, iOS and Windows. Sprites,
    # GPUWidgets and CameraView are portable C++ on top of it.
    add_subdirectory(Camera)
    add_subdirectory(GPU)
    add_subdirectory(GPUWidgets)

    # Glyph rasterization + atlas caching, on top of the GPU module.
    add_subdirectory(Text)
    add_subdirectory(Sprites)
    add_subdirectory(CameraView)

    # View->video recorder. The Snapshot tier is portable (AVFoundation on macOS,
    # Media Foundation on Windows); the Screen tier is ScreenCaptureKit on macOS
    # and a stub on Windows until Windows.Graphics.Capture lands.
    if (NOT IOS)
        add_subdirectory(Video)
    endif ()
endif ()