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
