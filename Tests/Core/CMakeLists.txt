set(core_test_sources
        AllocationCount.cpp
        AppAffordanceTests.cpp
        AppTests.cpp
        ArgsTests.cpp
        AsyncCoroutineTests.cpp
        AsyncTests.cpp
        Base64Tests.cpp
        # Touches the real system clipboard, and restores whatever was there.
        ClipboardTests.cpp
        # Runs ConsoleHarness as a child: runConsole's exit codes and streams.
        ConsoleTests.cpp
        DynamicLibraryTests.cpp
        EventLoopTests.cpp
        FileLocationsTests.cpp
        FilePathNamesTests.cpp
        FilePathTests.cpp
        FileTests.cpp
        FileWatcherTests.cpp
        FileWriterTests.cpp
        FilesAppendTests.cpp
        FilesCopyMoveTests.cpp
        # Only the paths refused before anything is launched, and the targets
        # computed for the launch; nothing here starts a file manager.
        FilesOpenTests.cpp
        FilesTests.cpp
        FilesWildcardTests.cpp
        FormatTests.cpp
        MathsTests.cpp
        MemoryMappedFileTests.cpp
        PluginObjectTests.cpp
        ProcessInheritStdioTests.cpp
        ProcessTokeniseTests.cpp
        RangeTests.cpp
        ReloadableLibraryTests.cpp
        SourceWatcherTests.cpp
        StringsTests.cpp
        TemporaryFileTests.cpp
        ThreadTests.cpp
        TimerTests.cpp
        TimeTests.cpp
        ZipTests.cpp)

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
    list(APPEND core_test_sources
            BuildRunnerTests.cpp HotReloadLoopTests.cpp ReloadableTests.cpp
            ProcessTests.cpp)
endif ()

# The same options (verbatim commandLine, merged stderr, live output, working
# directory) through cmd.exe.
if (WIN32)
    list(APPEND core_test_sources ProcessTests-Windows.cpp)
endif ()

# The file/folder picker seam is Windows-only (App-Windows-FilePicker.h), and
# so are the wide-path conversions FilePathTests-Windows.cpp covers.
if (WIN32)
    list(APPEND core_test_sources
            FilePathTests-Windows.cpp
            FilePickerWindowsTests.cpp)
endif ()

# A hosted copy under a foreign host's own Win32 pump: attach, detach, attach
# again. The message-only window and its class are Windows' hosted machinery.
if (WIN32)
    list(APPEND core_test_sources HostedLoopTests-Windows.cpp)
endif ()

# Permission bits, symlinks and FIFOs — the file behaviour Windows has no form
# of. Each portable half covers the rest.
if (NOT WIN32)
    list(APPEND core_test_sources
            FilesTests-Posix.cpp
            MemoryMappedFileTests-Posix.cpp
            ModuleInfoTests-Posix.cpp)
endif ()

if (APPLE AND NOT IOS)
    list(APPEND core_test_sources AppTerminationTests.cpp)
endif ()

# Threads::addLoopSource and the hosted pump live in EventLoop-Linux.h and
# nowhere else.
if (LINUX)
    list(APPEND core_test_sources
            EventLoopSourceTests-Linux.cpp
            HostedLoopTests-Linux.cpp
            RootLoopTests-Linux.cpp)
endif ()

# $HOME and the XDG user-dirs file are how FilePath-Linux.cpp finds the
# per-user folders.
if (LINUX)
    list(APPEND core_test_sources FileLocationsTests-Linux.cpp)
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

# A resource shipped beside the executable, which is what
# Files::resourcesDirectory() has to find on every platform.
set(resource_marker_name ResourceMarker.txt)

add_custom_command(TARGET CoreTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_LIST_DIR}/${resource_marker_name}"
        "$<TARGET_FILE_DIR:CoreTests>/${resource_marker_name}")

target_compile_definitions(CoreTests PRIVATE
        EACP_TEST_RESOURCE_MARKER="${resource_marker_name}")

# The child Process/inheritStdio/streamsWhileRunning watches: it prints a
# marker to the stdout it inherited and lingers until the test kills it.
add_executable(StdioLingerHarness StdioLingerHarness.cpp)
target_link_libraries(StdioLingerHarness PRIVATE eacp-core)

target_compile_definitions(CoreTests PRIVATE
        EACP_STDIO_LINGER_HARNESS="$<TARGET_FILE:StdioLingerHarness>")
add_dependencies(CoreTests StdioLingerHarness)

# A command-line tool over Apps::runConsole and Args::Parser, which
# ConsoleTests runs to read the exit code and both streams it leaves.
add_executable(ConsoleHarness ConsoleHarness.cpp)
target_link_libraries(ConsoleHarness PRIVATE eacp-core)

target_compile_definitions(CoreTests PRIVATE
        EACP_CONSOLE_HARNESS="$<TARGET_FILE:ConsoleHarness>")
add_dependencies(CoreTests ConsoleHarness)

# The image a standalone executable reports for itself cannot be checked from
# inside the test binary: it depends on the path the launcher used, so the
# fixture is a second executable the cases start three different ways.
if (NOT WIN32)
    add_executable(ModuleInfoHarness ModuleInfoHarness.cpp)
    target_link_libraries(ModuleInfoHarness PRIVATE eacp-core)

    target_compile_definitions(CoreTests PRIVATE
            EACP_MODULE_INFO_HARNESS="$<TARGET_FILE:ModuleInfoHarness>")
    add_dependencies(CoreTests ModuleInfoHarness)
endif ()

# The fixture plugin's static initializer counts genuine loads in the
# process environment, so the tests can observe real unmaps.
eacp_add_plugin(DynamicLibraryTestPlugin DynamicLibraryTestPlugin.cpp)
target_link_libraries(DynamicLibraryTestPlugin PRIVATE eacp-core)

target_compile_definitions(CoreTests PRIVATE
        EACP_TEST_PLUGIN="$<TARGET_FILE:DynamicLibraryTestPlugin>")
add_dependencies(CoreTests DynamicLibraryTestPlugin)

# Builds of one module for ReloadableLibraryTests, HotReloadLoopTests and
# ReloadableTests, linking no eacp library: a bare module needs only
# PluginExport.h or ReloadableEntry.h, so it takes eacp-core's include directory
# and nothing else. ReloadFixtureC lacks the answer function, and
# ReloadableFixtureMismatch's entry has the wrong signature. Each one's path is
# EACP_<NAME_IN_CAPITALS>, so ReloadFixtureA is EACP_RELOAD_FIXTURE_A.
foreach (fixture
        ReloadFixtureA ReloadFixtureB ReloadFixtureC
        ReloadableFixtureA ReloadableFixtureB ReloadableFixtureMismatch
        ReloadableFixtureObjectA ReloadableFixtureObjectB ReloadableFixtureNull
        ReloadableFixtureThrow ReloadableFixturePair)
    eacp_add_plugin(${fixture} ${fixture}.cpp)
    target_include_directories(${fixture} PRIVATE
            $<TARGET_PROPERTY:eacp-core,INTERFACE_INCLUDE_DIRECTORIES>)

    string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" define ${fixture})
    string(TOUPPER ${define} define)

    target_compile_definitions(CoreTests PRIVATE
            EACP_${define}="$<TARGET_FILE:${fixture}>")
    add_dependencies(CoreTests ${fixture})
endforeach ()

# The thin-eacp-host bridge: a plugin whose own copy defers work, so the
# tests can watch this binary's root loop pump it (RootLoopTests-Linux.cpp).
if (LINUX)
    eacp_add_plugin(RootLoopTestPlugin RootLoopTestPlugin.cpp)
    target_link_libraries(RootLoopTestPlugin PRIVATE eacp-core)

    target_compile_definitions(CoreTests PRIVATE
            EACP_ROOT_LOOP_TEST_PLUGIN="$<TARGET_FILE:RootLoopTestPlugin>")
    add_dependencies(CoreTests RootLoopTestPlugin)
endif ()

# The system clipboard is one object shared with the whole machine, so the
# Clipboard group takes a lock that every other group copying to it takes too.
# The file is appended to TEST_INCLUDE_FILES after the one nano_discover_tests
# registers, so the group's entry exists by the time it runs.
if (NOT CMAKE_CROSSCOMPILING)
    set(clipboard_lock_file
            "${CMAKE_CURRENT_BINARY_DIR}/ClipboardSerialisation.cmake")

    file(WRITE "${clipboard_lock_file}"
            "set_tests_properties(Clipboard PROPERTIES\n"
            "        RESOURCE_LOCK eacp-system-clipboard)\n")

    set_property(DIRECTORY APPEND PROPERTY
            TEST_INCLUDE_FILES "${clipboard_lock_file}")
endif ()
