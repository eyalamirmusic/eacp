eacp_add_app(Transforms Main.cpp)
target_link_libraries(Transforms PRIVATE eacp-ui)
set_target_properties(Transforms PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Transforms)
set_default_target_setting(Transforms)
