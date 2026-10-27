nano_add_executable(CrashReportTests
        SOURCES CrashReportTests.cpp ReportDatabaseTests.cpp
        TARGETS eacp-crash-report)

if (NOT TARGET SelfDestruct)
    return ()
endif ()

nano_add_executable(CrashReportingTests
        SOURCES PendingReportsTests.cpp SelfDestructTests.cpp
        TARGETS eacp-crash-reporting)

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
