eacp_add_app(TextureAtlas Main.cpp DISPLAY_NAME "Texture Atlas")
target_link_libraries(TextureAtlas PRIVATE eacp-sprites)
set_target_properties(TextureAtlas PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(TextureAtlas)
set_default_target_setting(TextureAtlas)
