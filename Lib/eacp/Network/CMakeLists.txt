add_library(eacp-network STATIC)

target_sources(eacp-network PRIVATE
        HTTP/Http.cpp
        HTTP/HttpProtocol.cpp
        HTTPServer/HttpServer.cpp
        HTTPServer/HttpServerDispatcher.cpp
        HTTPServer/ResponseBuilders.cpp
        TCP/Connection.cpp)

target_link_libraries(eacp-network PUBLIC eacp-core)

if (APPLE)
    target_sources(eacp-network PRIVATE
            HTTP/Http.mm
            HTTPServer/HttpServer.mm
            TCP/Connection-Posix.cpp)
    target_link_libraries(eacp-network PRIVATE
            "-framework Foundation"
            "-framework CoreFoundation")
elseif (WIN32)
    target_sources(eacp-network PRIVATE
            HTTP/Http-Windows.cpp
            HTTPServer/HttpServer-Windows.cpp
            TCP/Connection-Windows.cpp)
    target_link_libraries(eacp-network PRIVATE windowsapp ws2_32)
elseif (UNIX AND NOT APPLE)
    target_sources(eacp-network PRIVATE
            HTTP/Http-Linux.cpp
            HTTPServer/HttpServer-Linux.cpp
            TCP/Connection-Posix.cpp)
    find_package(CURL REQUIRED)
    target_link_libraries(eacp-network PRIVATE CURL::libcurl)
endif ()

set_default_target_setting(eacp-network)
eacp_enable_unity_build(eacp-network)
add_ide_sources(eacp-network)

find_package(Miro REQUIRED)

add_library(eacp-network-json INTERFACE)
target_link_libraries(eacp-network-json INTERFACE eacp-network Miro)

add_library(eacp-network-rpc STATIC)

target_sources(eacp-network-rpc PRIVATE
        HTTPRpc/RpcServer.cpp
        HTTPRpc/RpcClient.cpp)

target_link_libraries(eacp-network-rpc PUBLIC eacp-network Miro)

set_default_target_setting(eacp-network-rpc)
eacp_enable_unity_build(eacp-network-rpc)
add_ide_sources(eacp-network-rpc)
