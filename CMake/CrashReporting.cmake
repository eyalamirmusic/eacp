# Links an executable to the crash reporter. The handler process is the app's
# own executable relaunched, so only an executable can carry it.
#
# Every build also gets its symbols, since a report's moduleUuid changes with
# each one: a <target>.dSYM beside the executable or bundle on macOS, a PDB in
# every configuration on Windows. SYMBOLS_DIR <dir> copies them there as
# <target>-<version> (the app's VERSION), or <target> when it has none.
#
# On Windows, eacp-windows-error-reporting-helper.dll is copied beside the
# executable (its PDB into SYMBOLS_DIR): Windows Error Reporting loads it for
# fast-fail and heap-corruption crashes, which no in-process handler sees, so
# those get a stack too. start() registers it from there.
#
#   eacp_enable_crash_reporting(<target> [SYMBOLS_DIR <dir>])
function(eacp_enable_crash_reporting target)
    cmake_parse_arguments(PARSE_ARGV 1 CRASH "" "SYMBOLS_DIR" "")
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
    eacp_add_crash_symbols(${target} "${CRASH_SYMBOLS_DIR}")

    if (TARGET eacp-windows-error-reporting-helper)
        eacp_add_windows_error_reporting_helper(${target} "${CRASH_SYMBOLS_DIR}")
    endif ()
endfunction()

function(eacp_add_windows_error_reporting_helper target symbols_dir)
    add_dependencies(${target} eacp-windows-error-reporting-helper)
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:eacp-windows-error-reporting-helper> $<TARGET_FILE_DIR:${target}>
            VERBATIM)

    if (symbols_dir)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory ${symbols_dir}
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:eacp-windows-error-reporting-helper>
                        ${symbols_dir}/eacp-windows-error-reporting-helper.pdb
                VERBATIM)
    endif ()
endfunction()

function(eacp_get_crash_symbols_name target out)
    get_target_property(version ${target} EACP_APP_VERSION)

    if (NOT version)
        get_target_property(version ${target} VERSION)
    endif ()

    if (version)
        set(${out} "${target}-${version}" PARENT_SCOPE)
    else ()
        set(${out} "${target}" PARENT_SCOPE)
    endif ()
endfunction()

function(eacp_add_crash_symbols target symbols_dir)
    eacp_get_crash_symbols_name(${target} symbols_name)

    if (APPLE AND NOT IOS)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release,MinSizeRel>:-g>)
        get_target_property(is_bundle ${target} MACOSX_BUNDLE)

        if (is_bundle)
            set(dsym "$<TARGET_BUNDLE_DIR:${target}>/../${target}.dSYM")
        else ()
            set(dsym "$<TARGET_FILE_DIR:${target}>/${target}.dSYM")
        endif ()

        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND dsymutil $<TARGET_FILE:${target}> -o ${dsym}
                VERBATIM)

        if (symbols_dir)
            add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E rm -rf
                            ${symbols_dir}/${symbols_name}.dSYM
                    COMMAND ${CMAKE_COMMAND} -E copy_directory ${dsym}
                            ${symbols_dir}/${symbols_name}.dSYM
                    VERBATIM)
        endif ()
    elseif (WIN32 AND MSVC)
        # CMake's defaults give Release and MinSizeRel neither debug info nor
        # /DEBUG; /OPT:REF,ICF restores what /DEBUG would turn off there.
        if (NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
            set_target_properties(${target} PROPERTIES
                    MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
        endif ()

        target_link_options(${target} PRIVATE
                "$<$<CONFIG:Release,MinSizeRel>:/DEBUG;/OPT:REF;/OPT:ICF>")

        if (symbols_dir)
            add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${symbols_dir}
                    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${target}>
                            ${symbols_dir}/${symbols_name}.pdb
                    VERBATIM)
        endif ()
    endif ()
endfunction()
