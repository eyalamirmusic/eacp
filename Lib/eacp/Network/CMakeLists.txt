add_library(eacp-network STATIC)

target_sources(eacp-network PRIVATE
        HTTP/Http.cpp
        HTTPServer/HttpServerDispatcher.cpp)

target_link_libraries(eacp-network PUBLIC eacp-core)

if (APPLE)
    target_sources(eacp-network PRIVATE
            HTTP/Http.mm
            HTTPServer/HttpServer.mm)
    target_link_libraries(eacp-network PRIVATE
            "-framework Foundation"
            "-framework CoreFoundation")
elseif (WIN32)
    target_sources(eacp-network PRIVATE
            HTTP/Http-Windows.cpp
            HTTPServer/HttpServer-Windows.cpp)
    target_link_libraries(eacp-network PRIVATE windowsapp ws2_32)
elseif (UNIX AND NOT APPLE)
    target_sources(eacp-network PRIVATE
            HTTP/Http-Linux.cpp
            HTTPServer/HttpServer-Linux.cpp)
    find_package(CURL REQUIRED)
    target_link_libraries(eacp-network PRIVATE CURL::libcurl)
endif ()

set_default_target_setting(eacp-network)
eacp_enable_unity_build(eacp-network)
add_ide_sources(eacp-network)
