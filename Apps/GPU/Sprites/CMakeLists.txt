eacp_add_app(Sprites Main.cpp
        DESCRIPTION "Batched sprites from a procedural image, through SpriteRenderer.")
target_link_libraries(Sprites PRIVATE eacp-sprites)
set_target_properties(Sprites PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(Sprites)
set_default_target_setting(Sprites)
