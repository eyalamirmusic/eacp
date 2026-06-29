nano_add_executable(RangeTests
        SOURCES RangeTests.cpp
        TARGETS eacp-core)

nano_add_executable(FileTests
        SOURCES FileTests.cpp
        TARGETS eacp-core)

nano_add_executable(SHA256Tests
        SOURCES SHA256Tests.cpp
        TARGETS eacp-core)

nano_add_executable(AppTests
        SOURCES AppTests.cpp
        TARGETS eacp-core)

nano_add_executable(AppAffordanceTests
        SOURCES AppAffordanceTests.cpp
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

# ProcessTests drive real POSIX commands (/bin/echo, /bin/cat, ...), so they
# only run on macOS and Linux.
if (NOT WIN32)
    nano_add_executable(ProcessTests
            SOURCES ProcessTests.cpp
            TARGETS eacp-core)
endif ()
