eacp_add_app(HotReloadGPU Main.cpp)
target_link_libraries(HotReloadGPU PRIVATE eacp-gpu)
set_target_properties(HotReloadGPU PROPERTIES MACOSX_BUNDLE TRUE)

eacp_add_hot_reload(HotReloadGPU HotReloadGPUViews SOURCES Views.cpp HOT_RELOAD ON)
target_link_libraries(HotReloadGPUViews PRIVATE eacp-gpu)

eacp_set_gui_subsystem(HotReloadGPU)
set_default_target_setting(HotReloadGPU)
