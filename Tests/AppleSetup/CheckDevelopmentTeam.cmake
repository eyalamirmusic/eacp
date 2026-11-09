# Configures Fixture/ for iOS with the Xcode generator once per scenario and
# checks the DEVELOPMENT_TEAM every build configuration in the generated
# project carries. eacp's own team is only a default: whatever the project
# names - on the command line, as a normal variable or a cache entry, before
# or after eacp's setup runs, empty included - is what Xcode signs with.
#
#   cmake -DEACP_CMAKE_DIR=<eacp>/CMake -DFIXTURE_DIR=<this dir>/Fixture
#         -DWORK_DIR=<scratch dir> -P CheckDevelopmentTeam.cmake

foreach (var EACP_CMAKE_DIR FIXTURE_DIR WORK_DIR)
    if (NOT ${var})
        message(FATAL_ERROR "${var} is required")
    endif ()
endforeach ()

set(eacp_team "LK9GL8NWU4")
set(failures 0)

# check(<name> <expected team, or EMPTY> [configure args...])
function(check name expected)
    if (expected STREQUAL "EMPTY")
        set(expected "\"\"")
    endif ()

    set(build_dir "${WORK_DIR}/${name}")
    file(REMOVE_RECURSE "${build_dir}")

    execute_process(
            COMMAND "${CMAKE_COMMAND}" -G Xcode
            -S "${FIXTURE_DIR}" -B "${build_dir}"
            -DCMAKE_SYSTEM_NAME=iOS
            "-DEACP_CMAKE_DIR=${EACP_CMAKE_DIR}"
            ${ARGN}
            RESULT_VARIABLE rc
            OUTPUT_VARIABLE out
            ERROR_VARIABLE out)

    if (NOT rc EQUAL 0)
        message(SEND_ERROR "${name}: configure failed (${rc})\n${out}")
        math(EXPR failures "${failures} + 1")
        set(failures ${failures} PARENT_SCOPE)
        return()
    endif ()

    file(READ "${build_dir}/EacpAppleSetupFixture.xcodeproj/project.pbxproj"
            pbxproj)
    # Without the closing semicolon, which would split the CMake list.
    string(REGEX MATCHALL "DEVELOPMENT_TEAM = [^;]*" settings "${pbxproj}")

    set(seen "")
    foreach (setting IN LISTS settings)
        string(REGEX REPLACE "DEVELOPMENT_TEAM = " "" value "${setting}")
        list(APPEND seen "${value}")
    endforeach ()
    list(REMOVE_DUPLICATES seen)

    if (NOT seen STREQUAL expected)
        message(SEND_ERROR
                "${name}: expected DEVELOPMENT_TEAM ${expected}, "
                "the generated project has '${seen}'")
        math(EXPR failures "${failures} + 1")
        set(failures ${failures} PARENT_SCOPE)
    else ()
        message(STATUS "${name}: DEVELOPMENT_TEAM ${expected}")
    endif ()
endfunction()

# Nothing named: eacp's default, so its own examples sign for a device.
check(Default ${eacp_team})
check(DefaultSetupInSubdir ${eacp_team} -DFIXTURE_SETUP_IN_SUBDIR=ON)

# The eacp-named knob replaces the default.
check(EacpVariable BBBBBBBBBB -DEACP_APPLE_DEVELOPMENT_TEAM=BBBBBBBBBB)

# The project's own team, however it names it, wins.
check(CommandLine AAAAAAAAAA -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=AAAAAAAAAA)
check(CommandLineEmpty EMPTY -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=)
check(NormalBefore CCCCCCCCCC -DFIXTURE_NORMAL_BEFORE=CCCCCCCCCC)
check(NormalAfter DDDDDDDDDD -DFIXTURE_NORMAL_AFTER=DDDDDDDDDD)
check(CacheAfter EEEEEEEEEE -DFIXTURE_CACHE_AFTER=EEEEEEEEEE)
check(CacheAfterSetupInSubdir FFFFFFFFFF
        -DFIXTURE_SETUP_IN_SUBDIR=ON -DFIXTURE_CACHE_AFTER=FFFFFFFFFF)

if (failures GREATER 0)
    message(FATAL_ERROR "${failures} development team scenario(s) failed")
endif ()
