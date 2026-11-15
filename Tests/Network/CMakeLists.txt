# Pure: no socket of the suite's own, so these build everywhere.
set(network_test_sources
        HttpRequestTests.cpp
        HttpUrlTests.cpp
        WebSocketProtocolTests.cpp)

# Every case in these starts a server or listener of its own, which a wasm
# program cannot: on the web the client is tested against a node server
# instead (the -Web sources below, Web/TestServer.mjs).
if (EACP_HAS_SOCKETS)
    list(APPEND network_test_sources
            HttpAsyncTests.cpp
            HttpCacheTests.cpp
            HttpDownloadProgressTests.cpp
            HttpJsonTests.cpp
            HttpParallelDownloadTests.cpp
            HttpRedirectTests.cpp
            HttpResponseSizeTests.cpp
            HttpResumeTests.cpp
            HttpRpcTests.cpp
            HttpServerTests.cpp
            HttpStreamingBodyTests.cpp
            HttpTimeoutTests.cpp
            HttpUserAgentTests.cpp
            OnlineResourceTests.cpp
            OnlineResourcesTests.cpp
            TcpConnectionTests.cpp
            WebSocketServerTests.cpp
            WebSocketTests.cpp)
endif ()

if (EMSCRIPTEN)
    list(APPEND network_test_sources
            HttpWebTests-Web.cpp
            OnlineResourceWebTests-Web.cpp
            WebSocketWebTests-Web.cpp)
endif ()

# The lock and channel cross-process cases drive a real second process, which
# iOS cannot spawn (Process-Default.cpp is a stub).
if (EACP_HAS_SOCKETS AND NOT IOS)
    list(APPEND network_test_sources
            IpcChannelTests.cpp
            IpcLockTests.cpp
            IpcMessengerTests.cpp
            IpcRpcTests.cpp)
endif ()

# eacp-network-rpc re-exports eacp-network, so naming that one too would put the
# archive on the link line twice and ld warns about the duplicate.
if (EACP_HAS_SOCKETS)
    set(network_test_targets eacp-network-json eacp-network-rpc)
else ()
    set(network_test_targets eacp-network Miro)
endif ()

nano_add_executable(NetworkTests
        SOURCES ${network_test_sources}
        TARGETS ${network_test_targets})

if (EACP_HAS_SOCKETS AND NOT IOS)
    add_executable(IpcLockHarness IpcLockHarness.cpp)
    target_link_libraries(IpcLockHarness PRIVATE eacp-network)

    add_executable(IpcChannelHarness IpcChannelHarness.cpp)
    target_link_libraries(IpcChannelHarness PRIVATE eacp-network)

    target_compile_definitions(NetworkTests PRIVATE
            EACP_IPC_LOCK_HARNESS="$<TARGET_FILE:IpcLockHarness>"
            EACP_IPC_CHANNEL_HARNESS="$<TARGET_FILE:IpcChannelHarness>")
    add_dependencies(NetworkTests IpcLockHarness IpcChannelHarness)
endif ()

# The node server the web cases talk to, started before the groups that need
# it and stopped after them. It writes its addresses to a state file, which the
# tests read through node's own file system.
if (EMSCRIPTEN)
    set(web_test_server "${CMAKE_CURRENT_SOURCE_DIR}/Web/TestServer.mjs")
    set(web_test_server_state "${CMAKE_CURRENT_BINARY_DIR}/web-test-server.json")

    target_compile_definitions(NetworkTests PRIVATE
            EACP_WEB_TEST_SERVER_STATE="${web_test_server_state}")

    add_test(NAME WebTestServerStart
            COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${web_test_server}"
            start "${web_test_server_state}")
    add_test(NAME WebTestServerStop
            COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${web_test_server}"
            stop "${web_test_server_state}")
    set_tests_properties(WebTestServerStart PROPERTIES
            FIXTURES_SETUP eacpWebTestServer)
    set_tests_properties(WebTestServerStop PROPERTIES
            FIXTURES_CLEANUP eacpWebTestServer)

    set_target_properties(NetworkTests PROPERTIES
            EACP_WEB_TEST_FIXTURE eacpWebTestServer
            EACP_WEB_TEST_FIXTURE_GROUPS "^Web(Http|OnlineResource|WebSocket)")

    # Quits with a fetch and a socket in flight, which must not call into the
    # exited runtime (Tests/CMakeLists.txt registers it).
    add_executable(QuitWithFetchHarness QuitWithFetchHarness-Web.cpp)
    target_link_libraries(QuitWithFetchHarness PRIVATE eacp-network)
    eacp_web_executable(QuitWithFetchHarness COMPANION "${web_test_server}")
endif ()
