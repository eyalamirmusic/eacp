add_executable(TextureAtlas Main.cpp)
target_link_libraries(TextureAtlas PRIVATE eacp-sprites)

set(BUNDLE_ID "com.eacp.textureatlas")

set_target_properties(TextureAtlas PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Texture Atlas"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(TextureAtlas)
set_default_target_setting(TextureAtlas)
