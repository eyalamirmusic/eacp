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
        EventLoopTests.cpp
        FileDragTests.cpp
        FramePacerTests.cpp
        GameInputTests.cpp
        ImageOpsTests.cpp
        ImageTests.cpp
        KeyCodeTests.cpp
        MagnifyTests.cpp
        MenuCommandsTests.cpp
        MenuTests.cpp
        RectTests.cpp
        RoundedRectTests.cpp
        ScrollWheelTests.cpp
        SizeConstraintTests.cpp
        TestMain.cpp
        TouchTests.cpp
        ViewVisibilityTests.cpp
        ViewWindowTests.cpp
        WindowTests.cpp)

# The ones that need a platform image codec or a 2D Context. Listed rather than
# left to self-skip: a suite that skips every case still reports as a pass.
# Android has a codec without a Context; desktop Linux has neither.
if (EACP_HAS_CONTEXT OR ANDROID)
    list(APPEND graphics_test_sources ImageCodecTests.cpp)
endif ()

if (EACP_HAS_CONTEXT)
    list(APPEND graphics_test_sources RenderToImageTests.cpp)
endif ()

# The two Linux tables with no portable half: the commands a Path records, and
# the framework's KeyCodes against the kernel's evdev numbering.
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
# decided by a host view that only exists here. NativeChildSurfaceTests-macOS
# is the same story from the other side -- where foreign content lands inside
# one of our layouts is a native frame or it is nothing.
if (APPLE AND NOT IOS)
    list(APPEND graphics_test_sources
            CursorTests-macOS.mm
            EmbeddedViewTests-macOS.mm
            FileDragTests-macOS.mm
            FileDropTests-macOS.mm
            KeyForwardingTests-macOS.mm
            MagnifyTests-macOS.mm
            MenuTests-macOS.mm
            NativeChildSurfaceTests-macOS.mm
            ScrollWheelTests-macOS.mm
            ViewVisibilityTests-macOS.mm
            WindowTests-macOS.mm)
endif ()

# The iOS twin of EmbeddedViewTests-macOS: the surface inside a host's UIView,
# which is what an AUv3's view controller hands an editor. CI builds it with
# the rest of the simulator build; nothing in CI runs it.
if (IOS)
    list(APPEND graphics_test_sources EmbeddedViewTests-iOS.mm)
endif ()

if (WIN32)
    list(APPEND graphics_test_sources
            CompositionPixelTests-Windows.cpp
            KeyForwardingTests-Windows.cpp
            MouseDeltaTests-Windows.cpp
            MouseWheelTests-Windows.cpp
            WindowGeometryTests-Windows.cpp
            WindowLayoutTests-Windows.cpp)
endif ()

nano_add_executable(GraphicsTests
        NO_MAIN
        SOURCES
            ${graphics_test_sources}
        TARGETS eacp-graphics)

if (APPLE AND NOT IOS)
    set_target_properties(GraphicsTests PROPERTIES OBJCXX_STANDARD 20)
    target_link_libraries(GraphicsTests PRIVATE "-framework AppKit")
elseif (IOS)
    set_target_properties(GraphicsTests PROPERTIES OBJCXX_STANDARD 20)
    target_link_libraries(GraphicsTests PRIVATE "-framework UIKit")
endif ()

# Four binaries of their own. The three window suites must NOT be headless,
# unlike GraphicsTests, and each needs the raw client library a case reaches
# for: WaylandWindowTests attaches a wl_shm buffer to a subsurface, and
# X11WindowTests and EmbeddedViewTests open a second connection so what they
# assert about a window comes from the server rather than from the backend
# under test, and drive real pointer and key input into it through XTest.
#
# EmbeddedViewTests and DisplayLinkTests have a plain main because they play
# host to the loop rather than running it: pumpEventLoop refuses re-entry, so a
# case inside Apps::run would pump nothing (plan.md stage 0's note, and D6 -
# an embedded surface never runs the loop it is in).
if (LINUX)
    nano_add_executable(WaylandWindowTests
            NO_MAIN
            SOURCES
                WaylandWindowTests-Linux.cpp
            TARGETS eacp-graphics)

    target_link_libraries(WaylandWindowTests PRIVATE eacp-wayland)

    nano_add_executable(X11WindowTests
            NO_MAIN
            SOURCES
                X11ClipboardTests-Linux.cpp
                X11WindowTests-Linux.cpp
            TARGETS eacp-graphics)

    # XTest is the tests' alone: it synthesises the pointer and key events the
    # input cases need, and nothing in the library ever asks a server to make
    # up input.
    include(FindPkgConfig)
    pkg_check_modules(EACP_XTEST IMPORTED_TARGET xcb-xtest)

    target_link_libraries(X11WindowTests PRIVATE eacp-x11)

    if (EACP_XTEST_FOUND)
        target_link_libraries(X11WindowTests PRIVATE PkgConfig::EACP_XTEST)
        target_compile_definitions(X11WindowTests PRIVATE EACP_HAS_XTEST=1)
    endif ()

    # The one suite that plays host to eacp: its own connection owns the parent
    # window the surface is embedded in, and XTest is how a click reaches it.
    nano_add_executable(EmbeddedViewTests
            NO_MAIN
            SOURCES
                EmbeddedViewTests-Linux.cpp
            TARGETS eacp-graphics)

    target_link_libraries(EmbeddedViewTests PRIVATE eacp-x11)

    if (EACP_XTEST_FOUND)
        target_link_libraries(EmbeddedViewTests PRIVATE PkgConfig::EACP_XTEST)
        target_compile_definitions(EmbeddedViewTests PRIVATE EACP_HAS_XTEST=1)
    endif ()

    nano_add_executable(DisplayLinkTests
            NO_MAIN
            SOURCES
                DisplayLinkTests-Linux.cpp
            TARGETS eacp-graphics)
endif ()

# The system clipboard is one object shared with the whole machine, so every
# group that copies to it takes the lock Tests/Core's Clipboard group takes.
# Everything that opens a window on the X11 lane takes one more, the embedded
# surfaces included: each of those maps a host window of its own. There is no
# window manager under Scripts/with-xvfb, so every window a group opens lands at
# the same place and over whatever is already there: the window underneath gets
# an Expose on its presenting child and repaints, which is a frame nobody asked
# for (Present/continuousModeKeepsPresenting counted them and failed). The
# display also has exactly one pointer, one keyboard and one focus, which the
# X11 cases drive through XTest and read back.
if (LINUX AND NOT CMAKE_CROSSCOMPILING)
    set(linux_lock_file "${CMAKE_CURRENT_BINARY_DIR}/LinuxSerialisation.cmake")

    file(WRITE "${linux_lock_file}"
            "set_tests_properties(Wayland PROPERTIES\n"
            "        RESOURCE_LOCK eacp-system-clipboard)\n"
            "set_tests_properties(X11 PROPERTIES\n"
            "        RESOURCE_LOCK \"eacp-linux-display;eacp-system-clipboard\")\n"
            "set_tests_properties(EmbeddedView PROPERTIES\n"
            "        RESOURCE_LOCK eacp-linux-display)\n")

    set_property(DIRECTORY APPEND PROPERTY
            TEST_INCLUDE_FILES "${linux_lock_file}")
endif ()
