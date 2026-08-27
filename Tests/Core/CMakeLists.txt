set(core_test_sources
        AllocationCount.cpp
        AppAffordanceTests.cpp
        AppTests.cpp
        AsyncCoroutineTests.cpp
        AsyncTests.cpp
        # Touches the real system clipboard, and restores whatever was there.
        ClipboardTests.cpp
        DynamicLibraryTests.cpp
        EventLoopTests.cpp
        FilePathTests.cpp
        FileTests.cpp
        FilesTests.cpp
        MemoryMappedFileTests.cpp
        PluginObjectTests.cpp
        ProcessInheritStdioTests.cpp
        RangeTests.cpp
        StringsTests.cpp)

# Cross-platform proof for captureOutput=false: redirect the test's own
# stdout to a file and assert a non-capturing child writes through to it,
# live, while a capturing child still doesn't.
if (WIN32)
    list(APPEND core_test_sources StdioCapture-Windows.cpp)
else ()
    list(APPEND core_test_sources StdioCapture-Posix.cpp)
endif ()

# ProcessTests drive real POSIX commands (/bin/echo, /bin/cat, ...), so they
# only run on macOS and Linux.
if (NOT WIN32)
    list(APPEND core_test_sources ProcessTests.cpp)
endif ()

# The file/folder picker seam is Windows-only (App-Windows-FilePicker.h).
if (WIN32)
    list(APPEND core_test_sources FilePickerWindowsTests.cpp)
endif ()

if (APPLE AND NOT IOS)
    list(APPEND core_test_sources AppTerminationTests.cpp)
endif ()

nano_add_executable(CoreTests
        SOURCES ${core_test_sources}
        TARGETS eacp-core)

if (APPLE AND NOT IOS)
    add_executable(AppTerminationHarness AppTerminationHarness.mm)
    target_link_libraries(AppTerminationHarness PRIVATE eacp-core)

    target_compile_definitions(CoreTests PRIVATE
            EACP_TERMINATION_HARNESS="$<TARGET_FILE:AppTerminationHarness>")
    add_dependencies(CoreTests AppTerminationHarness)
endif ()

# The fixture plugin's static initializer counts genuine loads in the
# process environment, so the tests can observe real unmaps.
eacp_add_plugin(DynamicLibraryTestPlugin DynamicLibraryTestPlugin.cpp)
target_link_libraries(DynamicLibraryTestPlugin PRIVATE eacp-core)

target_compile_definitions(CoreTests PRIVATE
        EACP_TEST_PLUGIN="$<TARGET_FILE:DynamicLibraryTestPlugin>")
add_dependencies(CoreTests DynamicLibraryTestPlugin)

# The system clipboard is a single global object shared with the whole machine,
# and NanoTest gives every case its own process, so these cannot overlap: under
# ctest -j one case's copyText lands between another's copyText and getText, and
# the reader sees the wrong string. A RESOURCE_LOCK serialises them against each
# other while the rest of the suite still runs in parallel. The names are read
# out of the source so a new case is covered without editing a list here.
#
# The file is appended to TEST_INCLUDE_FILES after the one nano_discover_tests
# registers, so the tests exist by the time these run. A name that is not
# there — the placeholder entry written before the first build — is ignored
# rather than an error, so only the cross-compiling case, which registers a
# single target-level test instead, needs a guard.
if (NOT CMAKE_CROSSCOMPILING)
    file(STRINGS ClipboardTests.cpp clipboard_test_lines
            REGEX "\"Clipboard/[^\"]+\"")

    set(clipboard_lock_file
            "${CMAKE_CURRENT_BINARY_DIR}/ClipboardSerialisation.cmake")
    file(WRITE "${clipboard_lock_file}" "")

    foreach (line IN LISTS clipboard_test_lines)
        string(REGEX MATCH "Clipboard/[^\"]+" clipboard_test "${line}")
        file(APPEND "${clipboard_lock_file}"
                "set_tests_properties(\"${clipboard_test}\" PROPERTIES\n"
                "        RESOURCE_LOCK eacp-system-clipboard)\n")
    endforeach ()

    set_property(DIRECTORY APPEND PROPERTY
            TEST_INCLUDE_FILES "${clipboard_lock_file}")
endif ()
