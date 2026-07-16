nano_add_executable(TcpConnectionTests
        SOURCES TcpConnectionTests.cpp
        TARGETS eacp-network)

nano_add_executable(HttpRequestTests
        SOURCES HttpRequestTests.cpp
        TARGETS eacp-network)

nano_add_executable(HttpServerTests
        SOURCES HttpServerTests.cpp
        TARGETS eacp-network)

nano_add_executable(HttpDownloadProgressTests
        SOURCES HttpDownloadProgressTests.cpp
        TARGETS eacp-network)

nano_add_executable(HttpParallelDownloadTests
        SOURCES HttpParallelDownloadTests.cpp
        TARGETS eacp-network)

nano_add_executable(HttpJsonTests
        SOURCES HttpJsonTests.cpp
        TARGETS eacp-network-json)

nano_add_executable(HttpRpcTests
        SOURCES HttpRpcTests.cpp
        TARGETS eacp-network-rpc)

# The lock's cross-process cases drive a real second process, which iOS cannot
# spawn (Process-iOS.cpp is a stub).
if (NOT IOS)
    add_executable(IpcLockHarness IpcLockHarness.cpp)
    target_link_libraries(IpcLockHarness PRIVATE eacp-network)

    nano_add_executable(IpcLockTests
            SOURCES IpcLockTests.cpp
            TARGETS eacp-network)
    target_compile_definitions(IpcLockTests PRIVATE
            EACP_IPC_LOCK_HARNESS="$<TARGET_FILE:IpcLockHarness>")
    add_dependencies(IpcLockTests IpcLockHarness)
endif ()
