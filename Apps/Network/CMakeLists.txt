set(CMAKE_FOLDER Examples/Network)

add_subdirectory(HTTP)
add_subdirectory(BasicHTTP)
add_subdirectory(HttpServer)
add_subdirectory(TcpClient)
add_subdirectory(TcpServer)

if (NOT IOS)
    add_subdirectory(HttpRpcDemo)
endif ()
