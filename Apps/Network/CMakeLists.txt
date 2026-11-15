set(CMAKE_FOLDER Examples/Network)

add_subdirectory(HTTP)
add_subdirectory(BasicHTTP)

# Servers, raw TCP and the RPC transports over them need sockets of our own.
if (EACP_HAS_SOCKETS)
    add_subdirectory(HttpServer)
    add_subdirectory(TcpClient)
    add_subdirectory(TcpServer)
    add_subdirectory(WebSocketDemo)

    if (NOT IOS)
        add_subdirectory(HttpRpcDemo)
    endif ()
endif ()
