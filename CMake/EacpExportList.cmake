# The Windows export list behind hot reload: the symbols an executable exports
# so a module can link against them, written by eacp-export-list-tool
# (Lib/eacp/ExportList/README.md) from the objects and archives it links.
#
#   eacp_add_export_list(<name>
#       DEF <file>                 the module-definition file to write
#       IMAGE <file name>          the image the exports live in, e.g. MyApp.exe
#       [TARGETS <target>...]      static libraries are read as their archives;
#                                  executables, shared and object libraries as
#                                  their objects
#       [OBJECTS <file>...]        further objects or archives
#       [IMPORT_LIBRARY <file>]    an import library made from DEF alone
#       [UNFILTERED])              CMake __create_def's rules only, to compare
#
# Adds the custom target <name>. DEF is rewritten only when the list changes,
# and IMPORT_LIBRARY is remade only when DEF is, so an edit that changes no
# export relinks nothing that links either. The build fails, naming the file,
# on an input it cannot read: a corrupt one, or one compiled with link-time
# optimisation, which must be off for everything listed. Windows only.
function(eacp_add_export_list name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG
            "UNFILTERED" "DEF;IMAGE;IMPORT_LIBRARY" "TARGETS;OBJECTS")

    if (NOT WIN32)
        message(FATAL_ERROR "eacp_add_export_list(${name}): Windows only")
    endif ()

    if (NOT TARGET eacp-export-list-tool)
        message(FATAL_ERROR "eacp_add_export_list(${name}): eacp-export-list-tool "
                "is not built (it is not, when cross-compiling)")
    endif ()

    foreach (required DEF IMAGE)
        if (NOT ARG_${required})
            message(FATAL_ERROR "eacp_add_export_list(${name}): ${required} is required")
        endif ()
    endforeach ()

    set(archives ${ARG_OBJECTS})
    set(objects "")

    foreach (target IN LISTS ARG_TARGETS)
        get_target_property(type ${target} TYPE)

        if (type STREQUAL "STATIC_LIBRARY")
            list(APPEND archives "$<TARGET_FILE:${target}>")
        elseif (type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            list(APPEND objects "$<TARGET_OBJECTS:${target}>")
        else ()
            message(FATAL_ERROR "eacp_add_export_list(${name}): ${target} is a "
                    "${type}, which has no objects to list")
        endif ()
    endforeach ()

    # Per configuration, so a multi-config generator writes one list each.
    set(stem "${CMAKE_CURRENT_BINARY_DIR}/${name}")

    if (CMAKE_CONFIGURATION_TYPES)
        string(APPEND stem "-$<CONFIG>")
    endif ()

    # TARGET_OBJECTS also lists an executable's compiled .rc, which is no COFF
    # object.
    set(inputs "${stem}-inputs.txt")
    file(GENERATE OUTPUT "${inputs}"
            CONTENT "$<JOIN:${archives};$<FILTER:${objects},INCLUDE,\\.obj$>,\n>\n")

    set(flags "")

    if (ARG_UNFILTERED)
        list(APPEND flags --unfiltered)
    endif ()

    add_custom_command(
            OUTPUT "${ARG_DEF}"
            COMMAND eacp-export-list-tool
                    --output "${ARG_DEF}"
                    --name "${ARG_IMAGE}"
                    --inputs "${inputs}"
                    ${flags}
            DEPENDS eacp-export-list-tool "${inputs}" ${archives} ${objects}
            COMMENT "Listing the exports of ${ARG_IMAGE}"
            VERBATIM)

    set(outputs "${ARG_DEF}")

    if (ARG_IMPORT_LIBRARY)
        eacp_export_list_machine(machine)
        cmake_path(REPLACE_EXTENSION ARG_IMPORT_LIBRARY LAST_ONLY .exp
                OUTPUT_VARIABLE exports_file)

        add_custom_command(
                OUTPUT "${ARG_IMPORT_LIBRARY}"
                BYPRODUCTS "${exports_file}"
                COMMAND "${CMAKE_AR}" /nologo "/def:${ARG_DEF}" "/machine:${machine}"
                        "/out:${ARG_IMPORT_LIBRARY}"
                DEPENDS "${ARG_DEF}"
                COMMENT "Making the import library of ${ARG_IMAGE}"
                VERBATIM)

        list(APPEND outputs "${ARG_IMPORT_LIBRARY}")
    endif ()

    add_custom_target(${name} DEPENDS ${outputs})
endfunction()

# The librarian's /machine: for the target architecture.
function(eacp_export_list_machine out_var)
    set(architecture "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")

    if (architecture STREQUAL "x64")
        set(${out_var} X64 PARENT_SCOPE)
    elseif (architecture STREQUAL "ARM64")
        set(${out_var} ARM64 PARENT_SCOPE)
    else ()
        message(FATAL_ERROR "eacp_add_export_list: no import library for the "
                "'${architecture}' architecture; x64 and ARM64 are supported")
    endif ()
endfunction()
