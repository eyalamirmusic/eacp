nano_add_executable(CrashReportTests
        SOURCES CrashReportTests.cpp ReportDatabaseTests.cpp
        TARGETS eacp-crash-report)

if (NOT TARGET eacp-crash-faults)
    return ()
endif ()

# The fault cases relaunch this binary with --crashing-process, which crashes
# for real, and read the report its handler wrote.
nano_add_executable(CrashReportingTests
        NO_MAIN
        SOURCES TestMain.cpp CrashingProcess.cpp ThreadStackProbe.cpp
                PendingReportsTests.cpp FaultReportTests.cpp FaultsTests.cpp
                StackOverflowTests.cpp LoadedLibraryTests.cpp
        TARGETS eacp-crash-faults)
eacp_enable_crash_reporting(CrashReportingTests)

# Runs the real start() over a database the tests fill, to see when
# pendingReportFound is delivered.
add_executable(PendingReportsProbe PendingReportsProbe/Main.cpp)
eacp_enable_crash_reporting(PendingReportsProbe)
target_compile_definitions(CrashReportingTests PRIVATE
        EACP_PENDING_REPORTS_PROBE="$<TARGET_FILE:PendingReportsProbe>")
add_dependencies(CrashReportingTests PendingReportsProbe)

# Test-only: a library the crashing process loads at runtime and crashes in.
eacp_add_plugin(CrashingLibrary CrashingLibrary/CrashingLibrary.cpp)
target_link_libraries(CrashingLibrary PRIVATE eacp-core)
eacp_add_crash_symbols(CrashingLibrary "")
target_compile_definitions(CrashReportingTests PRIVATE
        EACP_CRASHING_LIBRARY="$<TARGET_FILE:CrashingLibrary>")
add_dependencies(CrashReportingTests CrashingLibrary)
