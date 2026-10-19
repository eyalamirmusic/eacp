eacp_add_app(HotReload Main.cpp)
target_link_libraries(HotReload PRIVATE eacp-ui)
set_target_properties(HotReload PROPERTIES MACOSX_BUNDLE TRUE)
eacp_add_hot_reload(HotReload MODULE HotReloadLayout SOURCES Layout.cpp HOT_RELOAD ON)

eacp_set_gui_subsystem(HotReload)
set_default_target_setting(HotReload)
