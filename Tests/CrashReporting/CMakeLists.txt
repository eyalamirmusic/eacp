nano_add_executable(CrashReportingTests
        SOURCES CrashReportTests.cpp ReportDatabaseTests.cpp SelfDestructTests.cpp
        TARGETS eacp-crash-reporting)

# The fault cases crash the demo app for real and read the report its handler
# wrote.
target_compile_definitions(CrashReportingTests PRIVATE
        EACP_SELF_DESTRUCT="$<TARGET_FILE:SelfDestruct>")
add_dependencies(CrashReportingTests SelfDestruct)
