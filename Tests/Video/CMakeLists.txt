nano_add_executable(VideoTests
        NO_MAIN
        SOURCES
            FrameStreamTests.cpp
            PlayerTests.cpp
            RoundTripTests.cpp
            TestMain.cpp
        TARGETS eacp-video)
