set(CMAKE_FOLDER Examples)

add_subdirectory(Console)
add_subdirectory(Network)

if ((APPLE OR WIN32) AND EACP_BUILD_GRAPHICS)
    add_subdirectory(Graphics)
    add_subdirectory(SVG)
    add_subdirectory(GPU)

    if (EACP_BUILD_WEBVIEW)
        add_subdirectory(WebView)
        add_subdirectory(Camera)
        add_subdirectory(Mixed)
    endif ()
endif ()
