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
