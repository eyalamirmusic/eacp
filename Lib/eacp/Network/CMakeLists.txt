add_library(eacp-network STATIC)

# The client half builds everywhere: HTTP, WebSocket and OnlineResource.
target_sources(eacp-network PRIVATE
        HTTP/Http.cpp
        HTTP/HttpProtocol.cpp
        OnlineResource/OnlineResource.cpp
        OnlineResource/OnlineResources.cpp
        WebSocket/Protocol.cpp
        WebSocket/WebSocket.cpp)

# The blocking backend seam runs on a worker thread for the async calls and
# OnlineResource; without threads (the web) they sit on the platform's own
# asynchronous client instead.
if (EACP_HAS_THREADS)
    target_sources(eacp-network PRIVATE
            HTTP/HttpAsync.cpp
            HTTP/HttpParallel.cpp
            OnlineResource/OnlineResource-Threaded.cpp)
endif ()

# Everything that opens a socket of its own: the servers, TCP and IPC.
if (EACP_HAS_SOCKETS)
    target_sources(eacp-network PRIVATE
            HTTPServer/HttpServer.cpp
            HTTPServer/HttpServerDispatcher.cpp
            HTTPServer/ResponseBuilders.cpp
            HTTPServer/StreamingBody.cpp
            IPC/Channel.cpp
            IPC/Lock.cpp
            IPC/Messenger.cpp
            IPC/Names.cpp
            TCP/Connection.cpp
            TCP/Listener.cpp
            WebSocket/Server.cpp)
endif ()

target_link_libraries(eacp-network PUBLIC eacp-core)

target_compile_definitions(eacp-network PRIVATE
        EACP_VERSION_STRING="${eacp_VERSION}")

# Read by the Network.h umbrella alone, which leaves the socket headers out
# where they are not built.
if (EACP_HAS_SOCKETS)
    target_compile_definitions(eacp-network PUBLIC EACP_HAS_SOCKETS=1)
else ()
    target_compile_definitions(eacp-network PUBLIC EACP_HAS_SOCKETS=0)
endif ()

if (APPLE)
    target_sources(eacp-network PRIVATE
            HTTP/Http.mm
            HTTPServer/HttpServer.mm
            IPC/Channel-Apple.cpp
            IPC/Channel-Posix.cpp
            IPC/Lock-Posix.cpp
            TCP/Connection-Posix.cpp
            WebSocket/WebSocket.mm)
    target_link_libraries(eacp-network PRIVATE
            "-framework Foundation"
            "-framework CoreFoundation"
            "-framework Network")
elseif (WIN32)
    target_sources(eacp-network PRIVATE
            HTTP/Http-Windows.cpp
            HTTPServer/HttpServer-Threaded.cpp
            IPC/Channel-Windows.cpp
            IPC/Lock-Windows.cpp
            TCP/Connection-Windows.cpp
            WebSocket/WebSocket-Windows.cpp)
    target_link_libraries(eacp-network PRIVATE winhttp ws2_32 advapi32)
elseif (ANDROID)
    target_sources(eacp-network PRIVATE
            HTTP/Http-Android.cpp
            HTTPServer/HttpServer-Threaded.cpp
            IPC/Channel-Linux.cpp
            IPC/Channel-Posix.cpp
            IPC/Lock-Posix.cpp
            TCP/Connection-Posix.cpp
            WebSocket/WebSocket-Android.cpp)
elseif (EMSCRIPTEN)
    # Ahead of UNIX, which Emscripten also sets. The HTTP client is the
    # runtime's fetch() and the WebSocket client its WebSocket, each reached
    # through a JS library every executable that links this one takes.
    target_sources(eacp-network PRIVATE
            HTTP/Http-Web.cpp
            HTTP/HttpAsync-Web.cpp
            OnlineResource/OnlineResource-Web.cpp
            WebSocket/WebSocket-Web.cpp)

    foreach (library HTTP/Http-Web.js WebSocket/WebSocket-Web.js)
        set(web_library "${CMAKE_CURRENT_SOURCE_DIR}/${library}")
        target_link_options(eacp-network INTERFACE
                "SHELL:--js-library \"${web_library}\"")
        set_property(TARGET eacp-network APPEND PROPERTY
                INTERFACE_LINK_DEPENDS "${web_library}")
    endforeach ()
elseif (UNIX AND NOT APPLE)
    target_sources(eacp-network PRIVATE
            HTTP/Http-Linux.cpp
            HTTPServer/HttpServer-Threaded.cpp
            IPC/Channel-Linux.cpp
            IPC/Channel-Posix.cpp
            IPC/Lock-Posix.cpp
            TCP/Connection-Posix.cpp
            WebSocket/WebSocket-Linux.cpp)
    find_package(CURL ${EACP_FIND_QUIET} REQUIRED)
    target_link_libraries(eacp-network PRIVATE CURL::libcurl)
endif ()

set_default_target_setting(eacp-network)
eacp_enable_unity_build(eacp-network)
add_ide_sources(eacp-network)

# The server's JSON handlers, so a server of our own is needed.
if (EACP_HAS_SOCKETS)
    add_library(eacp-network-json STATIC)

    target_sources(eacp-network-json PRIVATE
            HTTPServer/Json.cpp)

    target_link_libraries(eacp-network-json PUBLIC eacp-network Miro)

    set_default_target_setting(eacp-network-json)
    eacp_enable_unity_build(eacp-network-json)
    add_ide_sources(eacp-network-json)
endif ()

# The execution glue every Miro transport shares builds everywhere, since the
# WebView page bridge sits on it; the HTTP and IPC transports need sockets.
add_library(eacp-network-rpc STATIC)

target_sources(eacp-network-rpc PRIVATE
        Rpc/AsyncCommand.cpp)

if (EACP_HAS_SOCKETS)
    target_sources(eacp-network-rpc PRIVATE
            HTTPRpc/RpcServer.cpp
            HTTPRpc/RpcClient.cpp
            IPCRpc/RpcServer.cpp
            IPCRpc/RpcClient.cpp)
endif ()

target_link_libraries(eacp-network-rpc PUBLIC eacp-network Miro)

set_default_target_setting(eacp-network-rpc)
eacp_enable_unity_build(eacp-network-rpc)
add_ide_sources(eacp-network-rpc)
