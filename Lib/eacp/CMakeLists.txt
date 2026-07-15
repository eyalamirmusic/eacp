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
    add_subdirectory(Sprites)
    add_subdirectory(CameraView)

    # View->video recorder (AVFoundation + ScreenCaptureKit). macOS only for now
    # (uses AppKit/ScreenCaptureKit; iOS would drop the Screen tier).
    if (APPLE AND NOT IOS)
        add_subdirectory(Video)
    endif ()
endif ()