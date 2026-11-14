eacp_add_app(MenuBarDemo Main.cpp)
target_link_libraries(MenuBarDemo PRIVATE eacp-ui)
set_target_properties(MenuBarDemo PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(MenuBarDemo)
set_default_target_setting(MenuBarDemo)
