add_executable(SVG Main.cpp)
target_link_libraries(SVG PRIVATE eacp-svg)

set(BUNDLE_ID "com.eacp.svg")

set_target_properties(SVG PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "SVG App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(SVG)
