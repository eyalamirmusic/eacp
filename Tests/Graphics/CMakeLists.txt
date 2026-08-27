# Rect's y-axis contract (RectTests). The splitters were y-up in a y-down
# framework, so removeFromTop returned the bottom slice; nothing caught it
# because the result is still a correctly sized rect that still draws.
#
# Color's constexpr contract (ColorTests): the static_asserts in there fail the
# build if a definition moves back out of line.
#
# Uniqueness and coverage of the KeyCode table (KeyCodeTests), which is
# hand-transcribed platform virtual key codes and therefore easy to typo.
#
# View->Image snapshots (RenderToImageTests: paint/layers/children/group-opacity
# read back as pixels): CoreGraphics on Apple, an off-screen Direct2D target on
# Windows.
#
# The portable half of a command-id menu bar (MenuCommandsTests): ids, labels,
# accelerator text and the id-to-action lookup. Runs everywhere on purpose --
# the Win32 calls it supports cannot be tested from a Mac, but they are a thin
# shell around this, and the ordering agreement between the two walks is where
# the real risk is.
set(graphics_test_sources
        ColorTests.cpp
        CursorTests.cpp
        DisplayTests.cpp
        ImageOpsTests.cpp
        ImageTests.cpp
        KeyCodeTests.cpp
        MenuCommandsTests.cpp
        MenuTests.cpp
        RectTests.cpp
        RenderToImageTests.cpp
        RoundedRectTests.cpp
        ScrollWheelTests.cpp
        TestMain.cpp
        ViewVisibilityTests.cpp
        ViewWindowTests.cpp
        WindowTests.cpp)

# The macOS-only halves of the cursor, menu and scroll-wheel suites. Each
# portable half covers the model; the .mm file next to it checks the backing
# AppKit object really implements the selector the model relies on --
# cursorUpdate:, validateMenuItem:, scrollWheel:. Miss any of those and the
# state is remembered perfectly and never acted on, without failing anything
# else. APPLE covers iOS, which has no AppKit, no menu bar and no pointing
# device, so the native halves are macOS-only; the portable halves still build
# everywhere.
if (APPLE AND NOT IOS)
    list(APPEND graphics_test_sources
            CursorTests-macOS.mm
            MenuTests-macOS.mm
            ScrollWheelTests-macOS.mm
            ViewVisibilityTests-macOS.mm
            WindowTests-macOS.mm)
endif ()

if (WIN32)
    list(APPEND graphics_test_sources
            CompositionPixelTests-Windows.cpp
            WindowGeometryTests-Windows.cpp)
endif ()

nano_add_executable(GraphicsTests
        NO_MAIN
        SOURCES
            ${graphics_test_sources}
        TARGETS eacp-graphics)

if (APPLE AND NOT IOS)
    set_target_properties(GraphicsTests PROPERTIES OBJCXX_STANDARD 20)
    target_link_libraries(GraphicsTests PRIVATE "-framework AppKit")
endif ()
