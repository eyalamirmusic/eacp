# Declares a runtime-loaded eacp plugin: a MODULE library that statically
# links its own eacp copy. Everything defaults to hidden visibility — here for
# the plugin's own sources, and via set_default_target_setting for the eacp
# libraries linked into it — so the module's export table contains only the C
# functions the plugin marks with EACP_PLUGIN_EXPORT (see
# Core/Plugins/PluginExport.h). Which functions those are is the app's own
# contract; eacp imposes none. A host built with a different eacp version can
# never bind against the plugin's eacp, and RTLD_LOCAL loading
# (eacp::Plugins::DynamicLibrary) seals the reverse direction.
#
#   eacp_add_plugin(MyPlugin Plugin.cpp)
#   target_link_libraries(MyPlugin PRIVATE eacp-core)
function(eacp_add_plugin target)
    add_library(${target} MODULE ${ARGN})

    set_target_properties(${target} PROPERTIES
            PREFIX ""
            POSITION_INDEPENDENT_CODE ON)

    set_default_target_setting(${target})
    eacp_enable_unity_build(${target})
endfunction()
