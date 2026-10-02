eacp_add_app(Texture Main.cpp)
target_link_libraries(Texture PRIVATE eacp-gpu)
set_target_properties(Texture PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Texture)
set_default_target_setting(Texture)
