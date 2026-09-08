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

# The two that need a platform image codec or a 2D Context. Listed rather than
# left to self-skip: a suite that skips every case still reports as a pass.
if (EACP_HAS_CONTEXT)
    list(APPEND graphics_test_sources
            ImageTests.cpp
            RenderToImageTests.cpp)
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

# Three binaries of their own. The two window suites must NOT be headless,
# unlike GraphicsTests, and each needs the raw client library a case reaches
# for: WaylandWindowTests attaches a wl_shm buffer to a subsurface, and
# X11WindowTests opens a second connection so what it asserts about a window
# comes from the server rather than from the backend under test, and drives
# real pointer and key input into it through XTest.
#
# DisplayLinkTests is headless and has a plain main because it plays host to
# the loop: pumpEventLoop refuses re-entry, so a case inside Apps::run would
# pump nothing (plan.md stage 0's note).
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

    nano_add_executable(DisplayLinkTests
            NO_MAIN
            SOURCES
                DisplayLinkTests-Linux.cpp
            TARGETS eacp-graphics)
endif ()

# The system clipboard is one object shared with the whole machine and NanoTest
# gives every case its own process, so the three cases that take the selection
# cannot overlap: under ctest -j one case's copy lands between another's copy
# and its read. A RESOURCE_LOCK serialises them against each other while the
# rest of the suite still runs in parallel, exactly as Tests/Core does for
# ClipboardTests, and the names are read out of the source so a new case is
# covered without editing a list here.
if (LINUX AND NOT CMAKE_CROSSCOMPILING)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/WaylandWindowTests-Linux.cpp")

    file(STRINGS WaylandWindowTests-Linux.cpp wayland_clipboard_test_lines
            REGEX "\"Wayland/clipboard[^\"]+\"")

    set(wayland_clipboard_lock_file
            "${CMAKE_CURRENT_BINARY_DIR}/WaylandClipboardSerialisation.cmake")
    file(WRITE "${wayland_clipboard_lock_file}" "")

    foreach (line IN LISTS wayland_clipboard_test_lines)
        string(REGEX MATCH "Wayland/clipboard[^\"]+" wayland_clipboard_test
                "${line}")
        file(APPEND "${wayland_clipboard_lock_file}"
                "set_tests_properties(\"${wayland_clipboard_test}\" PROPERTIES\n"
                "        RESOURCE_LOCK eacp-system-clipboard)\n")
    endforeach ()

    set_property(DIRECTORY APPEND PROPERTY
            TEST_INCLUDE_FILES "${wayland_clipboard_lock_file}")
endif ()

# Everything that opens a window on the X11 lane takes one lock. There is no
# window manager under Scripts/with-xvfb, so every window a case opens lands at
# the same place and over whatever is already there: the case underneath gets
# an Expose on its presenting child and repaints, which is a frame nobody asked
# for (Present/continuousModeKeepsPresenting counted them and failed). The
# display also has exactly one pointer, one keyboard and one focus, which the
# X11 cases drive through XTest and read back. The names come out of the
# sources, so a new case is covered without editing a list here, and the source
# files are configure dependencies so that a new or renamed one is picked up
# rather than left unlocked until the next reconfigure.
if (LINUX AND NOT CMAKE_CROSSCOMPILING)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/X11WindowTests-Linux.cpp")

    file(STRINGS X11WindowTests-Linux.cpp x11_test_lines
            REGEX "\"X11/[^\"]+\"")

    set(x11_lock_file "${CMAKE_CURRENT_BINARY_DIR}/X11Serialisation.cmake")
    file(WRITE "${x11_lock_file}" "")

    foreach (line IN LISTS x11_test_lines)
        string(REGEX MATCH "X11/[^\"]+" x11_test "${line}")

        file(APPEND "${x11_lock_file}"
                "set_tests_properties(\"${x11_test}\" PROPERTIES\n"
                "        RESOURCE_LOCK eacp-linux-display)\n")
    endforeach ()

    set_property(DIRECTORY APPEND PROPERTY
            TEST_INCLUDE_FILES "${x11_lock_file}")
endif ()
