eacp_add_app(AtlasCeiling Main.cpp)
target_link_libraries(AtlasCeiling PRIVATE eacp-ui)
set_target_properties(AtlasCeiling PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(AtlasCeiling)
set_default_target_setting(AtlasCeiling)
