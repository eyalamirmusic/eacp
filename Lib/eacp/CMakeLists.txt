add_subdirectory(Core)
add_subdirectory(Network)

if (APPLE OR WIN32)
    add_subdirectory(Graphics)
    add_subdirectory(WebView)
    add_subdirectory(SVG)
endif ()