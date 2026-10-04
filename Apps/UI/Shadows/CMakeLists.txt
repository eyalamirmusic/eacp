eacp_add_app(Shadows Main.cpp)
target_link_libraries(Shadows PRIVATE eacp-ui)
set_target_properties(Shadows PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Shadows)
set_default_target_setting(Shadows)
