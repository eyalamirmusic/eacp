add_executable(EmbeddedViewDemo Main.cpp)
target_link_libraries(EmbeddedViewDemo PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.embeddedviewdemo")

set_target_properties(EmbeddedViewDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Embedded View Demo"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(EmbeddedViewDemo)
