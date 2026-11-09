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
    get_target_property(module_links ${module} LINK_LIBRARIES)

    if (NOT module_links)
        set(module_links "")
    endif ()

    set(compile_only "")

    foreach (link IN LISTS module_links)
        if (NOT TARGET "${link}")
            continue()
        endif ()

        list(APPEND compile_only "$<COMPILE_ONLY:${link}>")
        get_target_property(imported ${link} IMPORTED)

        if (NOT imported)
            add_dependencies(${module} ${link})
        endif ()
    endforeach ()

    set_target_properties(${module} PROPERTIES LINK_LIBRARIES "${compile_only}")
    target_link_libraries(${app} PRIVATE ${module_links})

    get_target_property(module_sources ${module} SOURCES)
    get_target_property(module_dir ${module} SOURCE_DIR)
    set(watched "")

    foreach (source IN LISTS module_sources)
        if (source MATCHES "\\$<" OR source MATCHES "NOTFOUND$")
            continue()
        endif ()

        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY ${module_dir} NORMALIZE)
        list(APPEND watched ${source})
    endforeach ()

    list(JOIN watched "|" watched)
    target_compile_definitions(${app} PRIVATE EACP_HOT_RELOAD_SOURCES="${watched}")

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

# Deferred to the end of the configure, once the app's and the library's links
# are final. In dev mode the library is a module that links nothing: its links
# go to the app, which exports them, and reach the module compile-only.
#
#   eacp_add_hot_reload(MyApp MyAppLayout SOURCES Layout.cpp [HOT_RELOAD <bool>])
#   target_link_libraries(MyAppLayout PRIVATE eacp-ui)
function(eacp_add_hot_reload app library)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "HOT_RELOAD" "SOURCES")

    set(enabled OFF)

    if (EACP_HOT_RELOAD)
        set(enabled ON)
    endif ()

    if (DEFINED ARG_HOT_RELOAD AND NOT ARG_HOT_RELOAD STREQUAL "")
        if (ARG_HOT_RELOAD)
            set(enabled ON)
        else ()
            set(enabled OFF)
        endif ()
    endif ()

    if (IOS OR ANDROID OR WIN32)
        set(enabled OFF)
    endif ()

    # Same compiler, same eacp: an EACP_HOT_RELOAD_EXPORT entry point returning
    # a C++ class never meets a C caller, so clang's C-linkage warning is off
    # for both.
    set_target_properties(${app} PROPERTIES EACP_HOT_RELOAD_ABI ON)

    if (NOT enabled)
        message(STATUS "${app}: hot reload OFF")
        add_library(${library} STATIC ${ARG_SOURCES})
        set_default_warnings_level(${library})
        set_target_properties(${library} PROPERTIES EACP_HOT_RELOAD_ABI ON)
        target_link_libraries(${app} PRIVATE ${library})
        target_compile_definitions(${app} PRIVATE EACP_HOT_RELOAD=0)
        return()
    endif ()

    message(STATUS "${app}: hot reload ON")

    set_target_properties(${app} PROPERTIES ENABLE_EXPORTS ON)
    eacp_set_default_visibility(${app})

    eacp_add_plugin(${library} ${ARG_SOURCES})
    add_dependencies(${app} ${library})
    eacp_set_default_visibility(${library})
    set_target_properties(${library} PROPERTIES
            EACP_HOT_RELOAD_ABI ON
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

    if (APPLE)
        target_link_options(${app} PRIVATE LINKER:-export_dynamic)
        target_link_options(${library} PRIVATE LINKER:-undefined,dynamic_lookup)
    endif ()

    cmake_language(EVAL CODE "
        cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]]
                CALL eacp_link_hot_reload [[${app}]] [[${library}]])")

    target_compile_definitions(${app} PRIVATE
            EACP_HOT_RELOAD=1
            EACP_HOT_RELOAD_LIBRARY="$<TARGET_FILE:${library}>"
            EACP_HOT_RELOAD_CMAKE="${CMAKE_COMMAND}"
            EACP_HOT_RELOAD_BUILD_DIR="${CMAKE_BINARY_DIR}"
            EACP_HOT_RELOAD_CONFIG="$<CONFIG>"
            EACP_HOT_RELOAD_TARGET="${library}"
            EACP_HOT_RELOAD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()
