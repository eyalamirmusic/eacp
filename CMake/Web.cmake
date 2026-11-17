# The web build: wasm32 through Emscripten, in a browser or under node.
#
# Single-threaded: eacp is a guest on the browser's own loop, and a page needs
# nothing but its static files, no special headers. Exceptions are wasm's own,
# so every object in a link, dependencies included, is compiled with them,
# which is why this is included before any target exists.

add_compile_options(-fwasm-exceptions)
add_link_options(-fwasm-exceptions)

# A browser-capable executable's wasm embedded in its .js (-sSINGLE_FILE=1), so
# its page, and the gallery, run from a file:// URL with no server. OFF keeps
# the .wasm beside the .js, which a page can fetch only over http.
option(EACP_WEB_SINGLE_FILE "Embed each web app's wasm in its .js, for file://" ON)

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

# Headless Chrome, for the <app>Headless tests.
find_program(EACP_WEB_CHROME
        NAMES "Google Chrome" Chromium google-chrome google-chrome-stable chromium
              chromium-browser chrome
        PATHS "/Applications/Google Chrome.app/Contents/MacOS"
              "$ENV{HOME}/Applications/Google Chrome.app/Contents/MacOS"
              "/Applications/Chromium.app/Contents/MacOS"
              "$ENV{PROGRAMFILES}/Google/Chrome/Application"
              "$ENV{LOCALAPPDATA}/Google/Chrome/Application")

if (NOT EACP_WEB_CHROME)
    message(STATUS "eacp: no Chrome found, so ctest runs no app in a browser; "
            "-DEACP_WEB_CHROME=<path> names one")
endif ()

set(EACP_WEB_UNCAUGHT_EXCEPTION "${CMAKE_CURRENT_LIST_DIR}/Web/UncaughtException.js"
        CACHE INTERNAL "eacp web uncaught exception handling")

# eacp_web_link_options(<out_var> [NODE_FILESYSTEM])
#
# The link settings every web executable shares: a growable memory, a 1 MB
# stack, a runtime that exits with the code Apps::quit or main() gives it, and
# one that aborts with the message of a C++ exception nothing caught
# (Web/UncaughtException.js), as std::terminate would.
# NODE_FILESYSTEM puts node's own file system under the program, which is
# what the tests read and write; it makes the executable node-only.
function(eacp_web_link_options out_var)
    cmake_parse_arguments(WEB "NODE_FILESYSTEM" "" "" ${ARGN})

    set(options -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 -sSTACK_SIZE=1MB
            "SHELL:--post-js \"${EACP_WEB_UNCAUGHT_EXCEPTION}\"")

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
    cmake_parse_arguments(DISCOVER "HEADLESS" "" "" ${ARGN})
    set(run_prefix "")

    # HEADLESS runs each group as a page in headless Chrome with WebGPU (the
    # cases are still listed through node), for a suite that needs the GPU.
    # With no Chrome found, the suite registers nothing.
    if (DISCOVER_HEADLESS)
        if (NOT EACP_WEB_CHROME)
            return()
        endif ()

        set(run_prefix ${EACP_WEB_TEST_NODE} "${EACP_WEB_LAUNCHER}" headless
                "$<TARGET_FILE:${target}>" --webgpu --chrome "${EACP_WEB_CHROME}"
                --timeout 300 --)
    endif ()

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
set(run_prefix [==[${run_prefix}]==])
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
# needs (in browser mode the app's own <app>.html, which carries the app's
# WebGPU and JSPI checks), relays the app's output and exits with its code.
# The app gets the
# arguments in its EACP_WEB_LAUNCH_ARGUMENTS property and, when
# EACP_WEB_COMPANION names a node script, that script is started first
# (`node <script> serve`, printing a JSON line with a baseUrl) and the URL
# appended to them. Both are read when the command is made.
#
# An app linked with NESTED_PUMP needs JSPI, so its launcher, which runs it
# under its own node in node mode, is the test node rather than the emulator.
function(eacp_web_launch_command out_var target mode)
    get_target_property(companion ${target} EACP_WEB_COMPANION)
    get_target_property(arguments ${target} EACP_WEB_LAUNCH_ARGUMENTS)
    get_target_property(nested_pump ${target} EACP_WEB_NESTED_PUMP)
    set(node ${CMAKE_CROSSCOMPILING_EMULATOR})

    if (nested_pump)
        set(node ${EACP_WEB_TEST_NODE})
    endif ()

    set(command ${node} "${EACP_WEB_LAUNCHER}" ${mode} "$<TARGET_FILE:${target}>")

    if (companion)
        list(APPEND command --companion "${companion}")
    endif ()

    if (mode STREQUAL "headless" AND EACP_WEB_CHROME)
        list(APPEND command --chrome "${EACP_WEB_CHROME}")
    endif ()

    eacp_web_links_target(webgpu ${target} eacp-gpu)

    if (mode STREQUAL "headless" AND webgpu)
        list(APPEND command --webgpu)
    endif ()

    list(APPEND command --)

    if (arguments)
        list(APPEND command ${arguments})
    endif ()

    set(${out_var} ${command} PARENT_SCOPE)
endfunction()

# eacp_web_open_command(<out_var> <file>)
#
# Web/Launch.mjs opening <file> in the default browser as a file:// URL and
# returning at once: no server, nothing relayed.
function(eacp_web_open_command out_var file)
    set(${out_var} ${CMAKE_CROSSCOMPILING_EMULATOR} "${EACP_WEB_LAUNCHER}" open
            "${file}" PARENT_SCOPE)
endfunction()

# eacp_web_links_target(<out_var> <target> <library>)
#
# Whether <target> links <library>, directly or through what it links: the
# LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES of every target reached, aliases
# resolved, generator expressions and anything that is not a target skipped.
# It answers for the links made so far, so it is asked once a target's
# directory is done: from a deferred call, or from Tests, which come after
# Apps. A WebGPU app is one that links eacp-gpu, a console app one that links
# no eacp-graphics.
function(eacp_web_links_target out_var target library)
    set(pending ${target})
    set(visited "")

    while (NOT pending STREQUAL "")
        list(POP_FRONT pending current)

        if (current MATCHES "[$<>]" OR NOT TARGET "${current}")
            continue()
        endif ()

        get_target_property(aliased ${current} ALIASED_TARGET)

        if (aliased)
            set(current ${aliased})
        endif ()

        if (current IN_LIST visited)
            continue()
        endif ()

        if (current STREQUAL library)
            set(${out_var} TRUE PARENT_SCOPE)
            return()
        endif ()

        list(APPEND visited ${current})

        foreach (property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(links ${current} ${property})

            if (links)
                list(APPEND pending ${links})
            endif ()
        endforeach ()
    endwhile ()

    set(${out_var} FALSE PARENT_SCOPE)
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

    # A WebGPU app also fails on anything the device reported (WebGPU.js and
    # the backend print those as "WebGPU: ...").
    set(failures "Aborted;aborted:;Assertion failed;exitRuntime;after runtime exit")
    eacp_web_links_target(webgpu ${target} eacp-gpu)

    if (webgpu)
        list(APPEND failures "WebGPU: ")
    endif ()

    foreach (name mode IN ZIP_LISTS names modes)
        eacp_web_launch_command(command ${target} ${mode})
        add_test(NAME ${name}
                COMMAND ${command}
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>")
        set_tests_properties(${name} PROPERTIES
                FAIL_REGULAR_EXPRESSION "${failures}"
                TIMEOUT 60)
    endforeach ()
endfunction()

# eacp_web_executable(<target> [NODE_FILESYSTEM] [NESTED_PUMP] [CONSOLE]
#                     [NO_GALLERY] [DESCRIPTION <text>]
#                     [COMPANION <script.mjs>] [ARGUMENTS <arg>...])
#
# eacp_add_app calls this itself on the web, so an example needs no call of
# its own; a test harness made with add_executable does.
#
# An executable that can run in a browser (no NODE_FILESYSTEM) is linked
# -sSINGLE_FILE=1 under EACP_WEB_SINGLE_FILE, and gets a page of its own,
# <target>.html beside its .js (Web/Host/app.html with its description,
# flags, companion and ARGUMENTS written in), and <target>-run, which builds
# it and opens that page in the default browser and is not part of all, with
# COMPANION and ARGUMENTS kept as its properties for that target and
# eacp_web_add_app_tests. The page is opened from file:// when the wasm is in
# the .js and there is no companion, and served by Launch.mjs's browser mode
# otherwise. Both are made once the directory is done, when the executable's
# links are all known; test harnesses get them too, since their -run target
# opens the page.
#
# It is also listed in the gallery (web-gallery, below) with its description
# (DESCRIPTION, else eacp_add_app's), unless NO_GALLERY says it is a test
# harness.
#
# The flags are read from what the executable links rather than declared. One
# that links eacp-gpu is a WebGPU app: its headless runs give Chrome its GPU
# (Launch.mjs --webgpu), and its ctest entries fail on a device validation
# error. One that links no eacp-graphics is a console app, which the gallery
# shows as printing rather than drawing; CONSOLE says so of one that links
# eacp-graphics, as eacp-gpu does, and still opens no window.
#
# NESTED_PUMP links eacp-core-nested-pump, and with it JSPI, so blocking code
# written for the other platforms - httpRequest, OnlineResource::fetch, a
# runEventLoopFor in main() - runs unchanged, on an engine that has JSPI only.
function(eacp_web_executable target)
    cmake_parse_arguments(WEB "NODE_FILESYSTEM;NESTED_PUMP;CONSOLE;NO_GALLERY"
            "COMPANION;DESCRIPTION" "ARGUMENTS" ${ARGN})

    if (WEB_NESTED_PUMP)
        target_link_libraries(${target} PRIVATE eacp-core-nested-pump)
        set_target_properties(${target} PROPERTIES EACP_WEB_NESTED_PUMP TRUE)
    endif ()

    set(link_arguments "")

    if (WEB_NODE_FILESYSTEM)
        list(APPEND link_arguments NODE_FILESYSTEM)
    endif ()

    eacp_web_link_options(options ${link_arguments})
    target_link_options(${target} PRIVATE ${options})
    set_property(TARGET ${target} APPEND PROPERTY
            LINK_DEPENDS "${EACP_WEB_UNCAUGHT_EXCEPTION}")

    if (WEB_NODE_FILESYSTEM)
        return()
    endif ()

    if (EACP_WEB_SINGLE_FILE)
        target_link_options(${target} PRIVATE -sSINGLE_FILE=1)
    endif ()

    # EACP_WEB_APP_COMPANION is kept apart from EACP_WEB_COMPANION, which a
    # test may set afterwards for a server the app itself does not need.
    set_target_properties(${target} PROPERTIES
            EACP_WEB_LAUNCH_ARGUMENTS "${WEB_ARGUMENTS}"
            EACP_WEB_COMPANION "${WEB_COMPANION}"
            EACP_WEB_APP_COMPANION "${WEB_COMPANION}"
            EACP_WEB_CONSOLE "${WEB_CONSOLE}")

    if (WEB_DESCRIPTION)
        set_target_properties(${target} PROPERTIES
                EACP_APP_DESCRIPTION "${WEB_DESCRIPTION}")
    endif ()

    # A deferred call reads its arguments when it runs, so the name is
    # written into it now.
    cmake_language(EVAL CODE
            "cmake_language(DEFER CALL eacp_web_finish_app [==[${target}]==])")

    if (NOT WEB_NO_GALLERY)
        set_property(GLOBAL APPEND PROPERTY EACP_WEB_GALLERY_APPS ${target})
    endif ()
endfunction()

function(eacp_web_finish_app target)
    eacp_web_write_page(${target})
    get_target_property(companion ${target} EACP_WEB_COMPANION)

    if (EACP_WEB_SINGLE_FILE AND NOT companion)
        eacp_web_open_command(command
                "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_BASE_NAME:${target}>.html")
    else ()
        eacp_web_launch_command(command ${target} browser)
    endif ()

    add_custom_target(${target}-run
            COMMAND ${command}
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
            USES_TERMINAL
            VERBATIM)
    add_dependencies(${target}-run ${target})
endfunction()

set(EACP_WEB_HOST_DIR "${CMAKE_CURRENT_LIST_DIR}/Web/Host"
        CACHE INTERNAL "eacp web host page")

set(EACP_WEB_PAGE_TEMPLATE "${EACP_WEB_HOST_DIR}/app.html"
        CACHE INTERNAL "eacp web app page template")
set(EACP_WEB_INDEX_TEMPLATE "${EACP_WEB_HOST_DIR}/index.html"
        CACHE INTERNAL "eacp web gallery page template")
set(EACP_WEB_PRUNE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/Web/PruneGallery.cmake"
        CACHE INTERNAL "eacp web gallery pruning")
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY
        CMAKE_CONFIGURE_DEPENDS "${EACP_WEB_PAGE_TEMPLATE}" "${EACP_WEB_INDEX_TEMPLATE}")

# A JSON string that is also safe inside a <script> and inside file(GENERATE):
# < > & and $ are written as \u escapes, so no </script> and no $<...> can
# come out of a value.
function(eacp_web_json_string out_var text)
    string(REPLACE "\\" "\\\\" text "${text}")
    string(REPLACE "\"" "\\\"" text "${text}")
    string(REPLACE "\n" "\\n" text "${text}")
    string(REPLACE "\r" "\\r" text "${text}")
    string(REPLACE "\t" "\\t" text "${text}")
    string(REPLACE "<" "\\u003c" text "${text}")
    string(REPLACE ">" "\\u003e" text "${text}")
    string(REPLACE "&" "\\u0026" text "${text}")
    string(REPLACE "$" "\\u0024" text "${text}")
    set(${out_var} "\"${text}\"" PARENT_SCOPE)
endfunction()

# Text for HTML, with $ escaped too for the same file(GENERATE) reason.
function(eacp_web_html_escape out_var text)
    string(REPLACE "&" "&amp;" text "${text}")
    string(REPLACE "<" "&lt;" text "${text}")
    string(REPLACE ">" "&gt;" text "${text}")
    string(REPLACE "\"" "&quot;" text "${text}")
    string(REPLACE "'" "&#39;" text "${text}")
    string(REPLACE "$" "&#36;" text "${text}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

function(eacp_web_json_bool out_var value)
    if (value)
        set(${out_var} true PARENT_SCOPE)
    else ()
        set(${out_var} false PARENT_SCOPE)
    endif ()
endfunction()

# The app's settings as one JSON object, {name, page, script, description,
# webgpu, jspi, console, singleFile, companion, arguments}, with page and
# script after <prefix>: what its page is written with and what apps.json and
# the gallery's index.html list.
function(eacp_web_app_json out_var app prefix)
    get_target_property(description ${app} EACP_APP_DESCRIPTION)
    get_target_property(companion ${app} EACP_WEB_APP_COMPANION)
    get_target_property(arguments ${app} EACP_WEB_LAUNCH_ARGUMENTS)
    get_target_property(nested_pump ${app} EACP_WEB_NESTED_PUMP)
    get_target_property(console ${app} EACP_WEB_CONSOLE)
    eacp_web_links_target(webgpu ${app} eacp-gpu)

    if (NOT console)
        eacp_web_links_target(draws ${app} eacp-graphics)

        if (NOT draws)
            set(console TRUE)
        endif ()
    endif ()

    set(json_arguments "")

    foreach (argument IN LISTS arguments)
        eacp_web_json_string(item "${argument}")
        list(APPEND json_arguments "${item}")
    endforeach ()

    list(JOIN json_arguments ", " json_arguments)

    if (companion)
        get_filename_component(companion "${companion}" NAME)
    else ()
        set(companion "")
    endif ()

    if (NOT description)
        set(description "")
    endif ()

    # The file names are generator expressions, which the escaping would
    # break, so they are added after it.
    eacp_web_json_string(name "${app}")
    eacp_web_json_string(page "${prefix}")
    eacp_web_json_string(script "${prefix}")
    string(REGEX REPLACE "\"$" "$<TARGET_FILE_BASE_NAME:${app}>.html\"" page "${page}")
    string(REGEX REPLACE "\"$" "$<TARGET_FILE_NAME:${app}>\"" script "${script}")
    eacp_web_json_string(description "${description}")
    eacp_web_json_string(companion "${companion}")
    eacp_web_json_bool(webgpu "${webgpu}")
    eacp_web_json_bool(jspi "${nested_pump}")
    eacp_web_json_bool(console "${console}")
    eacp_web_json_bool(single_file "${EACP_WEB_SINGLE_FILE}")

    set(${out_var} "{\"name\": ${name}, \"page\": ${page}, \"script\": ${script},
     \"description\": ${description},
     \"webgpu\": ${webgpu}, \"jspi\": ${jspi}, \"console\": ${console},
     \"singleFile\": ${single_file},
     \"companion\": ${companion}, \"arguments\": [${json_arguments}]}"
            PARENT_SCOPE)
endfunction()

# Writes $<TARGET_FILE_DIR>/<app>.html from Web/Host/app.html: @VAR@
# placeholders (string(CONFIGURE ... @ONLY), which leaves JS's ${...} alone)
# filled with the app's name and description, HTML-escaped, and its settings
# as JSON, then file(GENERATE) for the directory and the script's name, which
# are per configuration. The escaping keeps any $<...> out of what is filled.
function(eacp_web_write_page target)
    get_target_property(description ${target} EACP_APP_DESCRIPTION)

    if (NOT description)
        set(description "")
    endif ()

    eacp_web_app_json(EACP_WEB_PAGE_CONFIG ${target} "")
    eacp_web_html_escape(EACP_WEB_PAGE_NAME "${target}")
    eacp_web_html_escape(EACP_WEB_PAGE_DESCRIPTION "${description}")
    file(READ "${EACP_WEB_PAGE_TEMPLATE}" template)
    string(CONFIGURE "${template}" page @ONLY)
    file(GENERATE
            OUTPUT "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_BASE_NAME:${target}>.html"
            CONTENT "${page}")
endfunction()

# Run once the top-level configure is done, so every app is registered and its
# links, which decide its flags, are all made: writes the manifest and the
# gallery's index.html (Web/Host/index.html with the manifest written into its
# @EACP_WEB_GALLERY_MANIFEST@, so the list needs no fetch and opens from
# file://), and adds web-gallery, which lays out ${CMAKE_BINARY_DIR}/web as a
# static site - index.html, apps.json, and <app>/<app>.{html,js} for each app,
# with its .wasm when EACP_WEB_SINGLE_FILE is off - and first removes whatever
# else is there (Web/PruneGallery.cmake); and web-gallery-run, which opens that
# index.html in the default browser. The list links to each app's own page,
# since the runtime keeps its state on a global Module and exits for good.
function(eacp_web_write_gallery)
    get_property(apps GLOBAL PROPERTY EACP_WEB_GALLERY_APPS)
    set(gallery "${CMAKE_BINARY_DIR}/web")
    set(manifest "${CMAKE_BINARY_DIR}/CMakeFiles/eacp-web-apps.json")
    set(index "${CMAKE_BINARY_DIR}/CMakeFiles/eacp-web-index.html")
    set(file_list "${CMAKE_BINARY_DIR}/CMakeFiles/eacp-web-gallery-files.txt")
    set(entries "")
    set(files "index.html\napps.json\n")
    set(commands COMMAND "${CMAKE_COMMAND}" -E make_directory "${gallery}"
            COMMAND "${CMAKE_COMMAND}" -DGALLERY=${gallery} -DFILES=${file_list}
            -P "${EACP_WEB_PRUNE_SCRIPT}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${index}" "${gallery}/index.html"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${manifest}" "${gallery}/apps.json")

    set(separator "")

    foreach (app IN LISTS apps)
        eacp_web_app_json(entry ${app} "${app}/")
        string(APPEND entries "${separator}    ${entry}")
        set(separator ",\n")
        set(base "$<TARGET_FILE_DIR:${app}>/$<TARGET_FILE_BASE_NAME:${app}>")
        set(app_files "${base}.html" "$<TARGET_FILE:${app}>")
        string(APPEND files "${app}/$<TARGET_FILE_BASE_NAME:${app}>.html\n"
                "${app}/$<TARGET_FILE_NAME:${app}>\n")

        if (NOT EACP_WEB_SINGLE_FILE)
            list(APPEND app_files "${base}.wasm")
            string(APPEND files "${app}/$<TARGET_FILE_BASE_NAME:${app}>.wasm\n")
        endif ()

        list(APPEND commands
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${gallery}/${app}"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${app_files} "${gallery}/${app}")
    endforeach ()

    set(EACP_WEB_GALLERY_MANIFEST "{\"apps\": [\n${entries}\n]}")
    file(GENERATE OUTPUT "${manifest}" CONTENT "${EACP_WEB_GALLERY_MANIFEST}\n")
    file(GENERATE OUTPUT "${file_list}" CONTENT "${files}")
    file(READ "${EACP_WEB_INDEX_TEMPLATE}" template)
    string(CONFIGURE "${template}" page @ONLY)
    file(GENERATE OUTPUT "${index}" CONTENT "${page}")

    add_custom_target(web-gallery ${commands}
            COMMENT "Laying out the web gallery in ${gallery}"
            VERBATIM)

    if (apps)
        add_dependencies(web-gallery ${apps})
    endif ()

    eacp_web_open_command(command "${gallery}/index.html")
    add_custom_target(web-gallery-run
            COMMAND ${command}
            USES_TERMINAL
            VERBATIM)
    add_dependencies(web-gallery-run web-gallery)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL eacp_web_write_gallery)
