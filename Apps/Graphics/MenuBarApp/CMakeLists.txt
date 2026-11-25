eacp_add_app(MenuBarApp Main.cpp
        BUNDLE_ID "com.eacp.menubarapp"
        DISPLAY_NAME "Menu Bar App")
target_link_libraries(MenuBarApp PRIVATE eacp-ui)
set_target_properties(MenuBarApp PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(MenuBarApp)
set_default_target_setting(MenuBarApp)
