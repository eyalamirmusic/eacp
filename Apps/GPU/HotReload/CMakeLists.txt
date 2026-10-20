eacp_add_app(HotReloadGPU Main.cpp)
target_link_libraries(HotReloadGPU PRIVATE eacp-gpu)
set_target_properties(HotReloadGPU PROPERTIES MACOSX_BUNDLE TRUE)
eacp_add_hot_reload(HotReloadGPU MODULE HotReloadGPUView SOURCES Views.cpp HOT_RELOAD ON)

eacp_set_gui_subsystem(HotReloadGPU)
set_default_target_setting(HotReloadGPU)
