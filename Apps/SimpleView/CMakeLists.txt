add_executable(SimpleView Main.cpp)
target_link_libraries(SimpleView PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.simpleview")

set_target_properties(SimpleView PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Simple View App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})



set_default_target_setting(SimpleView)