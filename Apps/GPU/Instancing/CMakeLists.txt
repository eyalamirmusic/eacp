eacp_add_app(Instancing Main.cpp)
target_link_libraries(Instancing PRIVATE eacp-gpu)
set_target_properties(Instancing PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Instancing)
set_default_target_setting(Instancing)
