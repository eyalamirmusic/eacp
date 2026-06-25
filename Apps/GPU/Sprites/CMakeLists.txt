add_executable(Sprites Main.cpp)
target_link_libraries(Sprites PRIVATE eacp-sprites)

set(BUNDLE_ID "com.eacp.sprites")

set_target_properties(Sprites PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Sprites"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Sprites)
set_default_target_setting(Sprites)
