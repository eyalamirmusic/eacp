nano_add_executable(RangeTests
        SOURCES RangeTests.cpp
        TARGETS eacp-core)

nano_add_executable(FileTests
        SOURCES FileTests.cpp
        TARGETS eacp-core)

nano_add_executable(AppTests
        SOURCES AppTests.cpp
        TARGETS eacp-core)

nano_add_executable(EventLoopTests
        SOURCES EventLoopTests.cpp
        TARGETS eacp-core)

nano_add_executable(AsyncTests
        SOURCES AsyncTests.cpp
        TARGETS eacp-core)

nano_add_executable(AsyncCoroutineTests
        SOURCES AsyncCoroutineTests.cpp
        TARGETS eacp-core)
