eacp_add_app(Images Main.cpp)
target_link_libraries(Images PRIVATE eacp-ui)
set_target_properties(Images PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Images)
set_default_target_setting(Images)
