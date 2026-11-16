eacp_add_app(RenderScale Main.cpp)
target_link_libraries(RenderScale PRIVATE eacp-gpu)
set_target_properties(RenderScale PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(RenderScale)
set_default_target_setting(RenderScale)
