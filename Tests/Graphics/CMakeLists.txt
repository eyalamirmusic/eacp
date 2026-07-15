nano_add_executable(ImageTests
        SOURCES ImageTests.cpp
        TARGETS eacp-graphics)

nano_add_executable(ImageOpsTests
        SOURCES ImageOpsTests.cpp
        TARGETS eacp-graphics)

nano_add_executable(RoundedRectTests
        SOURCES RoundedRectTests.cpp
        TARGETS eacp-graphics)

nano_add_executable(WindowTests
        NO_MAIN
        SOURCES
            WindowTests.cpp
            TestMain.cpp
        TARGETS eacp-graphics)

# View->Image snapshots (paint/layers/children/group-opacity read back as
# pixels) are wired on the Apple backend only; the Windows renderToImage is
# still a stub returning an invalid Image (see View-Windows.cpp), so these
# pixel assertions are Apple-only until the WIC/D2D off-screen target lands.
if (APPLE)
    nano_add_executable(RenderToImageTests
            NO_MAIN
            SOURCES
                RenderToImageTests.cpp
                TestMain.cpp
            TARGETS eacp-graphics)
endif ()

if(WIN32)
    nano_add_executable(CompositionPixelTests
            NO_MAIN
            SOURCES
                CompositionPixelTests-Windows.cpp
                TestMain.cpp
            TARGETS eacp-graphics)
endif()
