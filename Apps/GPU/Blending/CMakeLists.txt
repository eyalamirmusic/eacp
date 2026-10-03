eacp_add_app(Blending Main.cpp)
target_link_libraries(Blending PRIVATE eacp-gpu)
set_target_properties(Blending PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Blending)
set_default_target_setting(Blending)
