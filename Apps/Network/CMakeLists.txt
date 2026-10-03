set(CMAKE_FOLDER Examples/Network)

add_subdirectory(HTTP)
add_subdirectory(BasicHTTP)
add_subdirectory(HttpServer)
add_subdirectory(TcpClient)
add_subdirectory(TcpServer)
add_subdirectory(WebSocketDemo)

if (NOT IOS)
    add_subdirectory(HttpRpcDemo)
endif ()

# Draws its state through the platform's own 2D tier, which Linux has none of.
if (EACP_HAS_CONTEXT)
    add_subdirectory(ConnectivityDemo)
endif ()
