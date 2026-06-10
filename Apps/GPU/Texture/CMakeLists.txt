add_executable(Texture Main.cpp)
target_link_libraries(Texture PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.texture")

set_target_properties(Texture PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Texture"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(Texture)
