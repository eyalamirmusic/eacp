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

# The Windows Error Reporting helper DLL is listed per user in HKCU, one value per DLL path, and every
# crashing process lists the same one beside this binary; a group that takes it
# out while another group crashes would see the other's listing.
if (WIN32)
    set(wer_lock_file "${CMAKE_CURRENT_BINARY_DIR}/WerSerialisation.cmake")

    file(WRITE "${wer_lock_file}"
            "set_tests_properties(CrashReport CrashReporting PROPERTIES\n"
            "        RESOURCE_LOCK eacp-wer-module-listing)\n")

    set_property(DIRECTORY APPEND PROPERTY
            TEST_INCLUDE_FILES "${wer_lock_file}")
endif ()
