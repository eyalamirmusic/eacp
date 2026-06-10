nano_add_executable(ImageTests
        SOURCES ImageTests.cpp
        TARGETS eacp-graphics)

nano_add_executable(WindowTests
        NO_MAIN
        SOURCES
            WindowTests.cpp
            TestMain.cpp
        TARGETS eacp-graphics)
