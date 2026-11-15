# The web build: wasm32 through Emscripten, in a browser or under node.
#
# Single-threaded: eacp is a guest on the browser's own loop, and a page needs
# nothing but its static files, no special headers. Exceptions are wasm's own,
# so every object in a link, dependencies included, is compiled with them,
# which is why this is included before any target exists.

add_compile_options(-fwasm-exceptions)
add_link_options(-fwasm-exceptions)

# Node runs the apps (CMAKE_CROSSCOMPILING_EMULATOR), which emcmake names and
# the toolchain file alone takes from PATH. The test suites link JSPI for their
# nested pumps (eacp-core-nested-pump), which needs node 25 or later:
# EACP_WEB_TEST_NODE is the emulator when it has it, else Homebrew's node.
# Node 24 has it behind a flag, so it is named with it as a list,
# -DEACP_WEB_TEST_NODE="<node>;--experimental-wasm-jspi". An IDE started from
# the Dock has no shell PATH, and so may have no emulator: the test node is it.
if (NOT EACP_WEB_TEST_NODE)
    foreach (node IN ITEMS "${CMAKE_CROSSCOMPILING_EMULATOR}"
            /opt/homebrew/bin/node /usr/local/bin/node)
        if (NOT node)
            continue()
        endif ()

        execute_process(COMMAND ${node} -e
                "process.exit(typeof WebAssembly.Suspending === 'function' ? 0 : 1)"
                RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)

        if (result EQUAL 0)
            set(EACP_WEB_TEST_NODE ${node} CACHE STRING
                    "node, and its flags, for the JSPI-linked web test suites")
            break()
        endif ()
    endforeach ()
endif ()

if (NOT CMAKE_CROSSCOMPILING_EMULATOR AND EACP_WEB_TEST_NODE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${EACP_WEB_TEST_NODE} CACHE FILEPATH
            "Path to the emulator for the target system." FORCE)
endif ()

if (NOT EACP_WEB_TEST_NODE)
    message(WARNING "eacp: no node with JSPI (node 25 or later) found, so the "
            "web test suites cannot run; pass -DEACP_WEB_TEST_NODE=<node>")
    set(EACP_WEB_TEST_NODE "${CMAKE_CROSSCOMPILING_EMULATOR}")
endif ()

# Headless Chrome, for the <app>-headless targets and the <app>Headless tests.
find_program(EACP_WEB_CHROME
        NAMES "Google Chrome" Chromium google-chrome google-chrome-stable chromium
              chromium-browser chrome
        PATHS "/Applications/Google Chrome.app/Contents/MacOS"
              "$ENV{HOME}/Applications/Google Chrome.app/Contents/MacOS"
              "/Applications/Chromium.app/Contents/MacOS"
              "$ENV{PROGRAMFILES}/Google/Chrome/Application"
              "$ENV{LOCALAPPDATA}/Google/Chrome/Application")

if (NOT EACP_WEB_CHROME)
    message(STATUS "eacp: no Chrome found, so ctest runs no app in a browser "
            "and the <app>-headless targets fail; -DEACP_WEB_CHROME=<path> "
            "names one")
endif ()

# eacp_web_link_options(<out_var> [NODE_FILESYSTEM])
#
# The link settings every web executable shares: a growable memory, a 1 MB
# stack, and a runtime that exits with the code Apps::quit or main() gives it.
# NODE_FILESYSTEM puts node's own file system under the program, which is
# what the tests read and write; it makes the executable node-only.
function(eacp_web_link_options out_var)
    cmake_parse_arguments(WEB "NODE_FILESYSTEM" "" "" ${ARGN})

    set(options -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 -sSTACK_SIZE=1MB)

    if (WEB_NODE_FILESYSTEM)
        list(APPEND options -sNODERAWFS=1)
    endif ()

    set(${out_var} ${options} PARENT_SCOPE)
endfunction()

set(EACP_WEB_DISCOVER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/Web/DiscoverTests.cmake"
        CACHE INTERNAL "eacp web test discovery")

# eacp_web_discover_tests(<target>)
#
# NanoTest registers a cross-compiled suite as one ctest entry, since it cannot
# run the executable at build time. Through node ctest can, so this lists the
# cases when ctest reads the directory and registers one entry per group, as
# NANO_TEST_GROUP_BY PREFIX does natively (Web/DiscoverTests.cmake). The
# whole-suite entry NanoTest added is the caller's to ignore
# (CTEST_CUSTOM_TESTS_IGNORE): this runs ahead of it, so it cannot change it.
#
# A target whose EACP_WEB_TEST_FIXTURE property names a ctest fixture has it
# required by every group matching the regular expression in its
# EACP_WEB_TEST_FIXTURE_GROUPS property: how a suite gets a server that a wasm
# program, which cannot listen, needs started beside it.
function(eacp_web_discover_tests target)
    get_target_property(fixture ${target} EACP_WEB_TEST_FIXTURE)
    get_target_property(fixture_groups ${target} EACP_WEB_TEST_FIXTURE_GROUPS)

    if (NOT fixture)
        set(fixture "")
        set(fixture_groups "")
    endif ()

    get_target_property(directory ${target} SOURCE_DIR)
    get_target_property(binary_directory ${target} BINARY_DIR)
    set(script "${binary_directory}/${target}_web_tests.cmake")

    file(GENERATE OUTPUT "${script}" CONTENT
            "set(target [==[${target}]==])
set(node ${EACP_WEB_TEST_NODE})
set(executable [==[$<TARGET_FILE:${target}>]==])
set(fixture [==[${fixture}]==])
set(fixture_groups [==[${fixture_groups}]==])
set(cmake_command [==[${CMAKE_COMMAND}]==])
set(discover_script [==[${EACP_WEB_DISCOVER_SCRIPT}]==])
include(\"\${discover_script}\")
")

    set_property(DIRECTORY "${directory}" APPEND PROPERTY
            TEST_INCLUDE_FILES "${script}")
endfunction()

set(EACP_WEB_LAUNCHER "${CMAKE_CURRENT_LIST_DIR}/Web/Launch.mjs"
        CACHE INTERNAL "eacp web app launcher")

# eacp_web_launch_command(<out_var> <target> <node|browser|headless>)
#
# Web/Launch.mjs running the app in that mode: it serves the page a browser
# needs, relays the app's output and exits with its code. The app gets the
# arguments in its EACP_WEB_LAUNCH_ARGUMENTS property and, when
# EACP_WEB_COMPANION names a node script, that script is started first
# (`node <script> serve`, printing a JSON line with a baseUrl) and the URL
# appended to them. Both are read when the command is made.
function(eacp_web_launch_command out_var target mode)
    get_target_property(companion ${target} EACP_WEB_COMPANION)
    get_target_property(arguments ${target} EACP_WEB_LAUNCH_ARGUMENTS)
    set(command ${CMAKE_CROSSCOMPILING_EMULATOR} "${EACP_WEB_LAUNCHER}" ${mode}
            "$<TARGET_FILE:${target}>")

    if (companion)
        list(APPEND command --companion "${companion}")
    endif ()

    if (mode STREQUAL "headless" AND EACP_WEB_CHROME)
        list(APPEND command --chrome "${EACP_WEB_CHROME}")
    endif ()

    list(APPEND command --)

    if (arguments)
        list(APPEND command ${arguments})
    endif ()

    set(${out_var} ${command} PARENT_SCOPE)
endfunction()

# eacp_web_launch_targets(<target>)
#
# <target>-run under node, <target>-browser in the default browser and
# <target>-headless in headless Chrome, each building the app first and none
# part of all.
function(eacp_web_launch_targets target)
    set(suffixes run browser headless)
    set(modes node browser headless)

    foreach (suffix mode IN ZIP_LISTS suffixes modes)
        eacp_web_launch_command(command ${target} ${mode})
        add_custom_target(${target}-${suffix}
                COMMAND ${command}
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
                USES_TERMINAL
                VERBATIM)
        add_dependencies(${target}-${suffix} ${target})
    endforeach ()
endfunction()

# eacp_web_add_app_tests(<target>)
#
# An app that exits with its verdict as ctest entries: <target> under node,
# the path an app takes with no browser around, and <target>Headless in
# headless Chrome wherever configure found one. Either fails on the exit code,
# on not exiting within 60 s, and on what the runtime prints when its exit is aborted or something calls
# into it after it exited, which a debug build checks (ASSERTIONS).
function(eacp_web_add_app_tests target)
    set(names ${target})
    set(modes node)

    if (EACP_WEB_CHROME)
        list(APPEND names ${target}Headless)
        list(APPEND modes headless)
    endif ()

    foreach (name mode IN ZIP_LISTS names modes)
        eacp_web_launch_command(command ${target} ${mode})
        add_test(NAME ${name}
                COMMAND ${command}
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>")
        set_tests_properties(${name} PROPERTIES
                FAIL_REGULAR_EXPRESSION
                "Aborted;aborted:;Assertion failed;exitRuntime;after runtime exit"
                TIMEOUT 60)
    endforeach ()
endfunction()

# eacp_web_executable(<target> [NODE_FILESYSTEM] [COMPANION <script.mjs>]
#                     [ARGUMENTS <arg>...])
#
# An executable that can run in a browser (no NODE_FILESYSTEM) also gets the
# launch targets, with COMPANION and ARGUMENTS kept as its properties for
# eacp_web_add_app_tests.
function(eacp_web_executable target)
    cmake_parse_arguments(WEB "NODE_FILESYSTEM" "COMPANION" "ARGUMENTS" ${ARGN})

    set(link_arguments "")

    if (WEB_NODE_FILESYSTEM)
        list(APPEND link_arguments NODE_FILESYSTEM)
    endif ()

    eacp_web_link_options(options ${link_arguments})
    target_link_options(${target} PRIVATE ${options})

    if (WEB_NODE_FILESYSTEM)
        return()
    endif ()

    set_target_properties(${target} PROPERTIES
            EACP_WEB_LAUNCH_ARGUMENTS "${WEB_ARGUMENTS}"
            EACP_WEB_COMPANION "${WEB_COMPANION}")
    eacp_web_launch_targets(${target})
endfunction()
