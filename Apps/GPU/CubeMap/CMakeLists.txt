eacp_add_app(CubeMap Main.cpp)
target_link_libraries(CubeMap PRIVATE eacp-gpu)
set_target_properties(CubeMap PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(CubeMap)
set_default_target_setting(CubeMap)
