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
#
# The rounded-rect radius contract (RoundedRectTests): a corner bigger than the
# rect it rounds is out of contract for every path builder, so the radius is
# fitted before it leaves eacp.
set(graphics_test_sources
        ColorTests.cpp
        CursorTests.cpp
        DisplayTests.cpp
        ImageOpsTests.cpp
        KeyCodeTests.cpp
        MenuCommandsTests.cpp
        MenuTests.cpp
        RectTests.cpp
        RoundedRectTests.cpp
        ScrollWheelTests.cpp
        TestMain.cpp
        ViewVisibilityTests.cpp
        ViewWindowTests.cpp
        WindowTests.cpp)

# The two that need something the Linux build deliberately does not have, which
# is exactly what EACP_HAS_CONTEXT names. ImageTests round-trips PNG and JPEG
# through the platform codec, which is ImageIO or WIC and has no Linux
# counterpart; RenderToImageTests reads back pixels a 2D Context painted.
# Listed rather than left to self-skip: a suite that skips every case reports as
# a pass, which is exactly the silent green the plan warns about.
#
# RoundedRectTests used to be here too. It only needs clampedCornerRadius and a
# Path to build the shape into, and Path-Linux.cpp gives it both, so it runs
# everywhere now.
if (EACP_HAS_CONTEXT)
    list(APPEND graphics_test_sources
            ImageTests.cpp
            RenderToImageTests.cpp)
endif ()

# What a Path holds on Linux, which is the only place it is a record of
# commands rather than a CGPath or an ID2D1PathGeometry. The portable half is
# RoundedRectTests above; this pins the points the builders actually emit,
# because getHandle() is a promise to a tessellator that has not been written
# yet and nothing else would notice a curve going the wrong way round.
#
# KeyCodeTests-Linux is beside it for the same reason, over the other Linux
# table with no portable half: the framework's KeyCodes against the kernel's
# evdev numbering. It earns a file of its own because the seat that feeds that
# table does not exist on a headless compositor, so WaylandWindowTests below
# cannot reach a line of the input translation - and a transposed evdev number
# gives a key that works perfectly while reporting itself as a different one.
if (LINUX)
    list(APPEND graphics_test_sources
            KeyCodeTests-Linux.cpp
            PathTests-Linux.cpp)
endif ()

# The macOS-only halves of the cursor, menu and scroll-wheel suites. Each
# portable half covers the model; the .mm file next to it checks the backing
# AppKit object really implements the selector the model relies on --
# cursorUpdate:, validateMenuItem:, scrollWheel:. Miss any of those and the
# state is remembered perfectly and never acted on, without failing anything
# else. APPLE covers iOS, which has no AppKit, no menu bar and no pointing
# device, so the native halves are macOS-only; the portable halves still build
# everywhere.
#
# EmbeddedViewTests-macOS has no portable half: where a surface lands inside a
# host's view is the AppKit frame, and the y-axis conversion it turns on is
# decided by a host view that only exists here.
if (APPLE AND NOT IOS)
    list(APPEND graphics_test_sources
            CursorTests-macOS.mm
            EmbeddedViewTests-macOS.mm
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

# A binary of its own, because it is the one that must NOT be headless.
# GraphicsTests forces Apps::getAppEnvironment().headless before nano::run, and
# rightly: the model it covers is portable and a window on screen adds nothing
# to it. This one covers only what a compositor does, so it carries a main that
# leaves headless alone, every case self-skips when no compositor answers, and
# EACP_REQUIRE_DISPLAY=1 turns that skip into a failure.
#
# It links eacp-wayland directly, which nothing above eacp-graphics otherwise
# does: one case stands in for the swapchain by attaching a wl_shm buffer to a
# view's subsurface and committing it, which is the only way to make a frame
# callback arrive.
if (LINUX)
    nano_add_executable(WaylandWindowTests
            NO_MAIN
            SOURCES
                WaylandWindowTests-Linux.cpp
            TARGETS eacp-graphics)

    target_link_libraries(WaylandWindowTests PRIVATE eacp-wayland)
endif ()
