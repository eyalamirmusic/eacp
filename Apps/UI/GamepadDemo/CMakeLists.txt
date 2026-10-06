eacp_add_app(GamepadDemo Main.cpp DISPLAY_NAME "Gamepad Demo")
target_link_libraries(GamepadDemo PRIVATE eacp-ui)
set_target_properties(GamepadDemo PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(GamepadDemo)
set_default_target_setting(GamepadDemo)
