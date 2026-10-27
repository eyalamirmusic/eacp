add_library(eacp-crash-report STATIC)

target_link_libraries(eacp-crash-report PUBLIC eacp-core)

target_sources(eacp-crash-report PRIVATE
        CrashReport.cpp
        ReportDatabase.cpp)

set_default_target_setting(eacp-crash-report)
eacp_enable_unity_build(eacp-crash-report)

if (NOT ((APPLE AND NOT IOS) OR WIN32))
    return ()
endif ()

add_library(eacp-crash-reporting STATIC)

target_link_libraries(eacp-crash-reporting PUBLIC eacp-crash-report)

target_sources(eacp-crash-reporting PRIVATE
        CrashHandler.cpp
        CrashReporting.cpp
        HandlerReport.cpp
        SharedState.cpp)

if (APPLE)
    target_sources(eacp-crash-reporting PRIVATE
            CrashClient-macOS.cpp
            CrashHandlerProcess-macOS.cpp
            HangWatchdog-macOS.cpp
            MachImages-macOS.cpp
            UncaughtExceptions-macOS.mm)
    target_link_libraries(eacp-crash-reporting PRIVATE "-framework Foundation")
elseif (WIN32)
    target_sources(eacp-crash-reporting PRIVATE
            CrashClient-Windows.cpp
            CppExceptions-Windows.cpp
            CrashHandlerProcess-Windows.cpp
            ExceptionNames-Windows.cpp)
    target_link_libraries(eacp-crash-reporting PRIVATE dbghelp)
endif ()

set_default_target_setting(eacp-crash-reporting)
eacp_enable_unity_build(eacp-crash-reporting)
