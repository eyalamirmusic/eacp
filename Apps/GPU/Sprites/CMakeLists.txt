eacp_add_app(Sprites Main.cpp)
target_link_libraries(Sprites PRIVATE eacp-sprites)
set_target_properties(Sprites PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Sprites)
set_default_target_setting(Sprites)
