add_executable(GlyphAtlas Main.cpp)
target_link_libraries(GlyphAtlas PRIVATE eacp-sprites eacp-text)

set(BUNDLE_ID "com.eacp.glyphatlas")

set_target_properties(GlyphAtlas PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Glyph Atlas"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(GlyphAtlas)
set_default_target_setting(GlyphAtlas)
