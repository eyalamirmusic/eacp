eacp_add_app(GlyphAtlas Main.cpp
        BUNDLE_ID com.eacp.glyphatlas
        DISPLAY_NAME "Glyph Atlas")
target_link_libraries(GlyphAtlas PRIVATE eacp-sprites eacp-text)
set_target_properties(GlyphAtlas PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(GlyphAtlas)
set_default_target_setting(GlyphAtlas)
