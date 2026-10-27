# Links an executable to the crash reporter. The handler process is the app's
# own executable relaunched, so only an executable can carry it.
function(eacp_enable_crash_reporting target)
    get_target_property(type ${target} TYPE)

    if (NOT type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR
                "eacp_enable_crash_reporting: ${target} is a ${type}; the crash "
                "handler relaunches the app's own executable, so it needs one")
    endif ()

    if (NOT TARGET eacp-crash-reporting)
        message(FATAL_ERROR
                "eacp_enable_crash_reporting: crash reporting is built on macOS "
                "and Windows only")
    endif ()

    target_link_libraries(${target} PRIVATE eacp-crash-reporting)
    target_compile_definitions(${target} PUBLIC EACP_CRASH_REPORTING=1)
endfunction()
