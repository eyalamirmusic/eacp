eacp_add_plugin(HotReloadLayout Layout.cpp)
target_include_directories(HotReloadLayout PRIVATE
        $<TARGET_PROPERTY:eacp-core,INTERFACE_INCLUDE_DIRECTORIES>)

eacp_add_app(HotReload Main.cpp)
target_link_libraries(HotReload PRIVATE eacp-ui)
set_target_properties(HotReload PROPERTIES MACOSX_BUNDLE TRUE)
add_dependencies(HotReload HotReloadLayout)

target_compile_definitions(HotReload PRIVATE
        EACP_HOT_RELOAD_LIBRARY="$<TARGET_FILE:HotReloadLayout>"
        EACP_HOT_RELOAD_CMAKE="${CMAKE_COMMAND}"
        EACP_HOT_RELOAD_BUILD_DIR="${CMAKE_BINARY_DIR}"
        EACP_HOT_RELOAD_CONFIG="$<CONFIG>"
        EACP_HOT_RELOAD_TARGET="HotReloadLayout"
        EACP_HOT_RELOAD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

eacp_set_gui_subsystem(HotReload)
set_default_target_setting(HotReload)
