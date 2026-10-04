eacp_add_app(TouchDemo Main.cpp DISPLAY_NAME "Touch Demo")
target_link_libraries(TouchDemo PRIVATE eacp-ui)
set_target_properties(TouchDemo PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(TouchDemo)
set_default_target_setting(TouchDemo)
