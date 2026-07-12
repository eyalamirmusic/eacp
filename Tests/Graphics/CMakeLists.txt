nano_add_executable(ImageTests
        SOURCES ImageTests.cpp
        TARGETS eacp-graphics)

nano_add_executable(ImageOpsTests
        SOURCES ImageOpsTests.cpp
        TARGETS eacp-graphics)

nano_add_executable(WindowTests
        NO_MAIN
        SOURCES
            WindowTests.cpp
            TestMain.cpp
        TARGETS eacp-graphics)

if(WIN32)
    nano_add_executable(CompositionPixelTests
            NO_MAIN
            SOURCES
                CompositionPixelTests-Windows.cpp
                TestMain.cpp
            TARGETS eacp-graphics)
endif()
