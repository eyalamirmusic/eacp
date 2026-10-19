# A runtime-loaded eacp plugin: a MODULE library that statically links its
# own eacp copy. eacp and its dependencies compile with hidden visibility, so
# the module re-exports none of their symbols; plugin entry points are marked
# with EACP_PLUGIN_EXPORT (Core/Plugins/PluginExport.h), and the visibility
# of the plugin's own remaining code is the plugin author's choice.
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

#   eacp_add_hot_reload(MyApp MODULE MyAppLayout SOURCES Layout.cpp DEFAULT ON)
function(eacp_add_hot_reload app)
    cmake_parse_arguments(PARSE_ARGV 1 HOT_RELOAD "" "MODULE;DEFAULT" "SOURCES")

    set(enabled OFF)

    if (NOT EACP_HOT_RELOAD STREQUAL "")
        set(enabled ${EACP_HOT_RELOAD})
    elseif (DEFINED HOT_RELOAD_DEFAULT)
        set(enabled ${HOT_RELOAD_DEFAULT})
    endif ()

    if (IOS OR ANDROID)
        set(enabled OFF)
    endif ()

    target_sources(${app} PRIVATE ${HOT_RELOAD_SOURCES})

    if (NOT enabled)
        message(STATUS "${app}: hot reload OFF")
        target_compile_definitions(${app} PRIVATE EACP_HOT_RELOAD=0)
        return()
    endif ()

    message(STATUS "${app}: hot reload ON")

    eacp_add_plugin(${HOT_RELOAD_MODULE} ${HOT_RELOAD_SOURCES})
    target_include_directories(${HOT_RELOAD_MODULE} PRIVATE
            $<TARGET_PROPERTY:eacp-core,INTERFACE_INCLUDE_DIRECTORIES>)
    add_dependencies(${app} ${HOT_RELOAD_MODULE})

    target_compile_definitions(${app} PRIVATE
            EACP_HOT_RELOAD=1
            EACP_HOT_RELOAD_LIBRARY="$<TARGET_FILE:${HOT_RELOAD_MODULE}>"
            EACP_HOT_RELOAD_CMAKE="${CMAKE_COMMAND}"
            EACP_HOT_RELOAD_BUILD_DIR="${CMAKE_BINARY_DIR}"
            EACP_HOT_RELOAD_CONFIG="$<CONFIG>"
            EACP_HOT_RELOAD_TARGET="${HOT_RELOAD_MODULE}"
            EACP_HOT_RELOAD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()
