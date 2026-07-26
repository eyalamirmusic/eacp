nano_add_executable(VideoTests
        NO_MAIN
        SOURCES
            FrameStreamTests.cpp
            PlacementTests.cpp
            PlayerTests.cpp
            RoundTripTests.cpp
            TestMain.cpp
        TARGETS eacp-video-view)
