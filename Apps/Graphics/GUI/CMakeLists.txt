project(GUIApp VERSION 2.0.0)

eacp_add_app(GUI Main.cpp BUNDLE_ID "com.eacp.guiapp" DISPLAY_NAME "GUI App")
target_link_libraries(GUI PRIVATE eacp-ui)
set_target_properties(GUI PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(GUI)
set_default_target_setting(GUI)
