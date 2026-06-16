add_subdirectory(Core)
add_subdirectory(Network)

if ((APPLE OR WIN32) AND EACP_BUILD_GRAPHICS)
    add_subdirectory(Graphics)
    add_subdirectory(SVG)
    if (EACP_BUILD_WEBVIEW)
        add_subdirectory(WebView)
    endif ()
    # Camera (capture engine) builds everywhere graphics does — macOS, iOS and
    # Windows. CameraView needs the GPU/Sprites stack, which is not on iOS yet.
    add_subdirectory(Camera)

    if (NOT IOS)
        add_subdirectory(GPU)
        add_subdirectory(GPUWidgets)
        add_subdirectory(Sprites)
        add_subdirectory(CameraView)
    endif ()
endif ()