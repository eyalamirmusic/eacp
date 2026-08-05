set(network_test_sources
        HttpDownloadProgressTests.cpp
        HttpJsonTests.cpp
        HttpParallelDownloadTests.cpp
        HttpRequestTests.cpp
        HttpRpcTests.cpp
        HttpServerTests.cpp
        SseParserTests.cpp
        TcpConnectionTests.cpp
        WebSocketUrlTests.cpp)

# The lock and channel cross-process cases drive a real second process, which
# iOS cannot spawn (Process-iOS.cpp is a stub).
if (NOT IOS)
    list(APPEND network_test_sources
            IpcChannelTests.cpp
            IpcLockTests.cpp
            IpcMessengerTests.cpp
            IpcRpcTests.cpp)
endif ()

# eacp-network-rpc re-exports eacp-network, so naming that one too would put the
# archive on the link line twice and ld warns about the duplicate.
nano_add_executable(NetworkTests
        SOURCES ${network_test_sources}
        TARGETS eacp-network-json eacp-network-rpc)

if (NOT IOS)
    add_executable(IpcLockHarness IpcLockHarness.cpp)
    target_link_libraries(IpcLockHarness PRIVATE eacp-network)

    add_executable(IpcChannelHarness IpcChannelHarness.cpp)
    target_link_libraries(IpcChannelHarness PRIVATE eacp-network)

    target_compile_definitions(NetworkTests PRIVATE
            EACP_IPC_LOCK_HARNESS="$<TARGET_FILE:IpcLockHarness>"
            EACP_IPC_CHANNEL_HARNESS="$<TARGET_FILE:IpcChannelHarness>")
    add_dependencies(NetworkTests IpcLockHarness IpcChannelHarness)
endif ()
