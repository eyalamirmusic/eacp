nano_add_executable(CrashReportTests
        SOURCES CrashReportTests.cpp ReportDatabaseTests.cpp
        TARGETS eacp-crash-report)

if (NOT TARGET SelfDestruct)
    return ()
endif ()

nano_add_executable(CrashReportingTests
        SOURCES Matrix.cpp PendingReportsTests.cpp
                SelfDestructTests.cpp FaultsTests.cpp StackOverflowTests.cpp
        TARGETS eacp-crash-faults)

# The fault cases crash the demo app for real and read the report its handler
# wrote.
target_compile_definitions(CrashReportingTests PRIVATE
        EACP_SELF_DESTRUCT="$<TARGET_FILE:SelfDestruct>")
add_dependencies(CrashReportingTests SelfDestruct)

# Runs the real start() over a database the tests fill, to see when
# pendingReportFound is delivered.
add_executable(PendingReportsProbe PendingReportsProbe/Main.cpp)
eacp_enable_crash_reporting(PendingReportsProbe)
target_compile_definitions(CrashReportingTests PRIVATE
        EACP_PENDING_REPORTS_PROBE="$<TARGET_FILE:PendingReportsProbe>")
add_dependencies(CrashReportingTests PendingReportsProbe)

# Creates threads under each Options::stackOverflowReporting and overflows one.
# The baseline is the same program built without crash reporting, for the
# bench to compare against.
add_executable(StackOverflowProbe StackOverflowProbe/Main.cpp)
eacp_enable_crash_reporting(StackOverflowProbe)
target_compile_definitions(CrashReportingTests PRIVATE
        EACP_STACK_OVERFLOW_PROBE="$<TARGET_FILE:StackOverflowProbe>")
add_dependencies(CrashReportingTests StackOverflowProbe)

add_executable(StackOverflowBaseline StackOverflowProbe/Main.cpp)
target_link_libraries(StackOverflowBaseline PRIVATE eacp-core)
set_default_target_setting(StackOverflowBaseline)
set_default_target_setting(StackOverflowProbe)
