# The Process cases relaunch ProcessTests itself as their child
# (ProcessChildModes, dispatched from TestMain.cpp), so they need no shell.
set(process_test_sources
        ProcessChildModes.cpp
        ProcessConcurrentPipesTests.cpp
        ProcessInheritStdioTests.cpp
        ProcessStopTests.cpp
        ProcessTokeniseTests.cpp
        TestMain.cpp)

# requestStop() reaches a child through a different handler on each platform
# (StopRequest), and what it refuses or signals differs: CTRL_BREAK_EVENT to a
# process group sharing the console on Windows, SIGTERM elsewhere.
if (WIN32)
    list(APPEND process_test_sources
            ProcessStopTests-Windows.cpp StopRequest-Windows.cpp)
else ()
    list(APPEND process_test_sources
            ProcessStopTests-Posix.cpp StopRequest-Posix.cpp)
endif ()

# Cross-platform proof for captureOutput=false: redirect the test's own
# stdout to a file and assert a non-capturing child writes through to it,
# live, while a capturing child still doesn't.
if (WIN32)
    list(APPEND process_test_sources StdioCapture-Windows.cpp)
else ()
    list(APPEND process_test_sources StdioCapture-Posix.cpp)
endif ()

# ProcessTests.cpp also drives POSIX commands (/bin/echo, pwd) and signals; the
# Windows half runs the same options through cmd.exe.
if (WIN32)
    list(APPEND process_test_sources ProcessTests-Windows.cpp)
else ()
    list(APPEND process_test_sources ProcessTests.cpp)
endif ()

nano_add_executable(ProcessTests
        NO_MAIN
        SOURCES ${process_test_sources}
        TARGETS eacp-core)

# The child Process/inheritStdio/streamsWhileRunning watches: it prints a
# marker to the stdout it inherited and lingers until the test kills it.
add_executable(StdioLingerHarness StdioLingerHarness.cpp)
target_link_libraries(StdioLingerHarness PRIVATE eacp-core)

target_compile_definitions(ProcessTests PRIVATE
        EACP_STDIO_LINGER_HARNESS="$<TARGET_FILE:StdioLingerHarness>")
add_dependencies(ProcessTests StdioLingerHarness)
