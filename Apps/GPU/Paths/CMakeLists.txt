eacp_add_app(Paths Main.cpp)
target_link_libraries(Paths PRIVATE eacp-gpuwidgets)
set_target_properties(Paths PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Paths)
set_default_target_setting(Paths)
