# Registers one ctest entry per case group of a web suite, each running
# `<node> <suite>.js --test-group <group>`, or `<run_prefix> --test-group
# <group>` where the suite runs in a page (run_prefix is then its headless
# launch command). Included by the script eacp_web_discover_tests generates,
# which sets target, node, executable, fixture, fixture_groups, run_prefix,
# cmake_command and discover_script.
#
# A suite whose cases cannot be listed - not built, a crash, nothing listed -
# is one failing entry, <target>.discovery, rather than a quiet zero: that
# entry runs this file with -P and EACP_WEB_DISCOVERY_FAILURE, which fails
# with the reason.

if (DEFINED EACP_WEB_DISCOVERY_FAILURE)
    message(FATAL_ERROR "${EACP_WEB_DISCOVERY_FAILURE}")
endif ()

unset(failure)

if (NOT EXISTS "${executable}")
    set(failure "${target} is not built: ${executable}")
else ()
    execute_process(
            COMMAND ${node} "${executable}" --list-tests
            OUTPUT_VARIABLE listed
            ERROR_VARIABLE errors
            RESULT_VARIABLE result
            OUTPUT_STRIP_TRAILING_WHITESPACE)

    if (NOT result EQUAL 0)
        string(SUBSTRING "${errors}" 0 2000 errors)
        set(failure "${target} --list-tests failed (${result}): ${errors}")
    elseif (listed STREQUAL "")
        set(failure "${target} --list-tests listed no cases")
    endif ()
endif ()

if (DEFINED failure)
    string(REPLACE ";" "," failure "${failure}")
    add_test("${target}.discovery" "${cmake_command}"
            "-DEACP_WEB_DISCOVERY_FAILURE=${failure}" -P "${discover_script}")
    return()
endif ()

string(REPLACE "\n" ";" names "${listed}")
set(groups "")

foreach (name IN LISTS names)
    string(REGEX REPLACE "/.*" "" group "${name}")
    list(APPEND groups "${group}")
endforeach ()

list(REMOVE_DUPLICATES groups)

foreach (group IN LISTS groups)
    if (run_prefix STREQUAL "")
        add_test("${group}" ${node} "${executable}" --test-group "${group}")
    else ()
        add_test("${group}" ${run_prefix} --test-group "${group}")
    endif ()

    if (NOT fixture STREQUAL "" AND group MATCHES "${fixture_groups}")
        set_tests_properties("${group}" PROPERTIES FIXTURES_REQUIRED "${fixture}")
    endif ()
endforeach ()
