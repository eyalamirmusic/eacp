nano_add_executable(RangeTests
        SOURCES RangeTests.cpp
        TARGETS eacp-core)

nano_add_executable(FileTests
        SOURCES FileTests.cpp
        TARGETS eacp-core)

nano_add_executable(FilePathTests
        SOURCES FilePathTests.cpp
        TARGETS eacp-core)

nano_add_executable(AppTests
        SOURCES AppTests.cpp
        TARGETS eacp-core)

nano_add_executable(AppAffordanceTests
        SOURCES AppAffordanceTests.cpp
        TARGETS eacp-core)

if (APPLE AND NOT IOS)
    add_executable(AppTerminationHarness AppTerminationHarness.mm)
    target_link_libraries(AppTerminationHarness PRIVATE eacp-core)

    nano_add_executable(AppTerminationTests
            SOURCES AppTerminationTests.cpp
            TARGETS eacp-core)
    target_compile_definitions(AppTerminationTests PRIVATE
            EACP_TERMINATION_HARNESS="$<TARGET_FILE:AppTerminationHarness>")
    add_dependencies(AppTerminationTests AppTerminationHarness)
endif ()

# The fixture plugin's static initializer counts genuine loads in the
# process environment, so the tests can observe real unmaps.
eacp_add_plugin(DynamicLibraryTestPlugin DynamicLibraryTestPlugin.cpp)
target_link_libraries(DynamicLibraryTestPlugin PRIVATE eacp-core)

nano_add_executable(DynamicLibraryTests
        SOURCES DynamicLibraryTests.cpp
        TARGETS eacp-core)
target_compile_definitions(DynamicLibraryTests PRIVATE
        EACP_TEST_PLUGIN="$<TARGET_FILE:DynamicLibraryTestPlugin>")
add_dependencies(DynamicLibraryTests DynamicLibraryTestPlugin)

nano_add_executable(EventLoopTests
        SOURCES EventLoopTests.cpp
        TARGETS eacp-core)

nano_add_executable(AsyncTests
        SOURCES AsyncTests.cpp
        TARGETS eacp-core)

nano_add_executable(AsyncCoroutineTests
        SOURCES AsyncCoroutineTests.cpp
        TARGETS eacp-core)

# ProcessTests drive real POSIX commands (/bin/echo, /bin/cat, ...), so they
# only run on macOS and Linux.
if (NOT WIN32)
    nano_add_executable(ProcessTests
            SOURCES ProcessTests.cpp
            TARGETS eacp-core)
endif ()

# The file/folder picker seam is Windows-only (App-Windows-FilePicker.h).
if (WIN32)
    nano_add_executable(FilePickerWindowsTests
            SOURCES FilePickerWindowsTests.cpp
            TARGETS eacp-core)
endif ()
