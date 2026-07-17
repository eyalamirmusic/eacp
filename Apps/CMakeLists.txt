set(CMAKE_FOLDER Examples)

add_subdirectory(Console)
add_subdirectory(Network)

if ((APPLE OR WIN32) AND EACP_BUILD_GRAPHICS)
    add_subdirectory(Plugins)
    add_subdirectory(Graphics)
    add_subdirectory(SVG)
    add_subdirectory(GPU)

    # PTY backend is POSIX-only so far; Windows (ConPTY) comes next.
    if (APPLE AND NOT IOS)
        add_subdirectory(Terminal)
    endif ()

    if (EACP_BUILD_WEBVIEW)
        add_subdirectory(WebView)
        add_subdirectory(Camera)
        add_subdirectory(Mixed)
    endif ()
endif ()
