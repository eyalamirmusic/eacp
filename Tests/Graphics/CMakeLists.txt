nano_add_executable(ImageTests
        SOURCES ImageTests.cpp
        TARGETS eacp-graphics)

nano_add_executable(ImageOpsTests
        SOURCES ImageOpsTests.cpp
        TARGETS eacp-graphics)

# Rect's y-axis contract. The splitters were y-up in a y-down framework, so
# removeFromTop returned the bottom slice; nothing caught it because the result
# is still a correctly sized rect that still draws.
nano_add_executable(RectTests
        SOURCES RectTests.cpp
        TARGETS eacp-graphics)

nano_add_executable(RoundedRectTests
        SOURCES RoundedRectTests.cpp
        TARGETS eacp-graphics)

# Color's constexpr contract: the static_asserts in here fail the build if a
# definition moves back out of line.
nano_add_executable(ColorTests
        SOURCES ColorTests.cpp
        TARGETS eacp-graphics)

# Uniqueness and coverage of the KeyCode table, which is hand-transcribed
# platform virtual key codes and therefore easy to typo.
nano_add_executable(KeyCodeTests
        SOURCES KeyCodeTests.cpp
        TARGETS eacp-graphics)

nano_add_executable(WindowTests
        NO_MAIN
        SOURCES
            WindowTests.cpp
            TestMain.cpp
        TARGETS eacp-graphics)

# View->Image snapshots (paint/layers/children/group-opacity read back as
# pixels): CoreGraphics on Apple, an off-screen Direct2D target on Windows.
nano_add_executable(RenderToImageTests
        NO_MAIN
        SOURCES
            RenderToImageTests.cpp
            TestMain.cpp
        TARGETS eacp-graphics)

# Wheel-event delivery. The portable half covers the event shape and routing;
# the macOS file additionally checks the backing NSView really implements
# scrollWheel:, since inheriting NSResponder's no-op version drops every event
# without failing anything else.
# APPLE covers iOS, which has no AppKit and no pointing device to scroll with,
# so the native half is macOS-only. The portable half still builds everywhere.
set(scroll_wheel_sources ScrollWheelTests.cpp TestMain.cpp)

if (APPLE AND NOT IOS)
    list(APPEND scroll_wheel_sources ScrollWheelTests-macOS.mm)
endif ()

nano_add_executable(ScrollWheelTests
        NO_MAIN
        SOURCES
            ${scroll_wheel_sources}
        TARGETS eacp-graphics)

if (APPLE AND NOT IOS)
    set_target_properties(ScrollWheelTests PROPERTIES OBJCXX_STANDARD 20)
    target_link_libraries(ScrollWheelTests PRIVATE "-framework AppKit")
endif ()

if(WIN32)
    nano_add_executable(CompositionPixelTests
            NO_MAIN
            SOURCES
                CompositionPixelTests-Windows.cpp
                TestMain.cpp
            TARGETS eacp-graphics)
endif()
