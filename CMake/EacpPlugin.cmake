# A runtime-loaded eacp plugin: a MODULE library that statically links its
# own eacp copy. eacp and its dependencies compile with hidden visibility, so
# the module re-exports none of their symbols; plugin entry points are marked
# with EACP_PLUGIN_EXPORT (Core/Plugins/PluginExport.h), and the visibility
# of the plugin's own remaining code is the plugin author's choice. A build
# with eacp_add_hot_reload ON compiles eacp at default visibility instead, so
# plugins built in it re-export eacp and lose that isolation.
#
#   eacp_add_plugin(MyPlugin Plugin.cpp)
#   target_link_libraries(MyPlugin PRIVATE eacp-core)
function(eacp_add_plugin target)
    add_library(${target} MODULE ${ARGN})

    set_target_properties(${target} PROPERTIES
            PREFIX ""
            # CMake defines <target>_EXPORTS for a MODULE by default. eacp marks
            # its plugin entry points with EACP_PLUGIN_EXPORT and nothing reads
            # the generated macro, so drop it rather than leave every plugin
            # carrying a define the shared PCH image cannot match.
            DEFINE_SYMBOL ""
            POSITION_INDEPENDENT_CODE ON)

    # Keep the plugin's own sources free of STB_GNU_UNIQUE symbols so glibc can
    # unmap it on the last close -- see the eacp-wide flag in the root
    # CMakeLists that covers the eacp libraries linked in here.
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE -fno-gnu-unique)
    endif ()

    set_default_target_setting(${target})
    eacp_enable_unity_build(${target})
endfunction()

function(eacp_collect_link_targets target out_var)
    set(found ${${out_var}})
    get_target_property(links ${target} LINK_LIBRARIES)
    get_target_property(interface_links ${target} INTERFACE_LINK_LIBRARIES)

    foreach (link IN LISTS links interface_links)
        string(REGEX REPLACE "\\$<LINK_ONLY:([^>]*)>" "\\1" link "${link}")

        if (NOT TARGET "${link}")
            continue()
        endif ()

        get_target_property(aliased ${link} ALIASED_TARGET)

        if (aliased)
            set(link ${aliased})
        endif ()

        get_target_property(imported ${link} IMPORTED)

        if (imported OR link IN_LIST found)
            continue()
        endif ()

        list(APPEND found ${link})
        eacp_collect_link_targets(${link} found)
    endforeach ()

    set(${out_var} ${found} PARENT_SCOPE)
endfunction()

function(eacp_set_default_visibility target)
    set_target_properties(${target} PROPERTIES
            C_VISIBILITY_PRESET default
            CXX_VISIBILITY_PRESET default
            OBJC_VISIBILITY_PRESET default
            OBJCXX_VISIBILITY_PRESET default
            VISIBILITY_INLINES_HIDDEN OFF)
endfunction()

function(eacp_link_hot_reload app module)
    get_target_property(direct ${app} LINK_LIBRARIES)
    set(closure "")
    eacp_collect_link_targets(${app} closure)
    set(archives "")

    foreach (target IN LISTS closure)
        get_target_property(type ${target} TYPE)

        if (type STREQUAL "STATIC_LIBRARY")
            list(APPEND archives ${target})
        endif ()

        if (type MATCHES "^(STATIC|OBJECT)_LIBRARY$")
            eacp_set_default_visibility(${target})
        endif ()
    endforeach ()

    set(links "")

    foreach (link IN LISTS direct)
        if (TARGET "${link}")
            target_link_libraries(${module} PRIVATE $<COMPILE_ONLY:${link}>)
        endif ()

        if (NOT link IN_LIST archives)
            list(APPEND links "${link}")
        endif ()
    endforeach ()

    foreach (archive IN LISTS archives)
        set(whole "$<LINK_LIBRARY:WHOLE_ARCHIVE,${archive}>")

        if (archive IN_LIST direct)
            list(APPEND links "${whole}")
        else ()
            list(APPEND links "$<LINK_ONLY:${whole}>")
        endif ()

        set_target_properties(${app} PROPERTIES
                LINK_LIBRARY_OVERRIDE_${archive} WHOLE_ARCHIVE)
    endforeach ()

    set_target_properties(${app} PROPERTIES
            LINK_LIBRARIES "${links}"
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
endfunction()

# Deferred to the end of the configure, once the app's links are final.
#
#   eacp_add_hot_reload(MyApp MODULE MyAppLayout SOURCES Layout.cpp
#           [HOT_RELOAD ON|OFF])
function(eacp_add_hot_reload app)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "MODULE;HOT_RELOAD" "SOURCES")

    set(enabled ${EACP_HOT_RELOAD})

    if (DEFINED ARG_HOT_RELOAD)
        if (NOT ARG_HOT_RELOAD MATCHES "^(ON|OFF)$")
            message(FATAL_ERROR "eacp_add_hot_reload(${app}): HOT_RELOAD is ON "
                    "or OFF, not '${ARG_HOT_RELOAD}'")
        endif ()

        set(enabled ${ARG_HOT_RELOAD})
    endif ()

    if (IOS OR ANDROID OR WIN32)
        set(enabled OFF)
    endif ()

    target_sources(${app} PRIVATE ${ARG_SOURCES})

    if (NOT enabled)
        message(STATUS "${app}: hot reload OFF")
        target_compile_definitions(${app} PRIVATE EACP_HOT_RELOAD=0)
        return()
    endif ()

    message(STATUS "${app}: hot reload ON")

    set_target_properties(${app} PROPERTIES ENABLE_EXPORTS ON)
    eacp_set_default_visibility(${app})

    eacp_add_plugin(${ARG_MODULE} ${ARG_SOURCES})
    add_dependencies(${app} ${ARG_MODULE})
    eacp_set_default_visibility(${ARG_MODULE})
    set_target_properties(${ARG_MODULE} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

    if (APPLE)
        target_link_options(${app} PRIVATE LINKER:-export_dynamic)
        target_link_options(${ARG_MODULE} PRIVATE
                LINKER:-undefined,dynamic_lookup)
    endif ()

    cmake_language(EVAL CODE "
        cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]]
                CALL eacp_link_hot_reload [[${app}]] [[${ARG_MODULE}]])")

    target_compile_definitions(${app} PRIVATE
            EACP_HOT_RELOAD=1
            EACP_HOT_RELOAD_LIBRARY="$<TARGET_FILE:${ARG_MODULE}>"
            EACP_HOT_RELOAD_CMAKE="${CMAKE_COMMAND}"
            EACP_HOT_RELOAD_BUILD_DIR="${CMAKE_BINARY_DIR}"
            EACP_HOT_RELOAD_CONFIG="$<CONFIG>"
            EACP_HOT_RELOAD_TARGET="${ARG_MODULE}"
            EACP_HOT_RELOAD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()
