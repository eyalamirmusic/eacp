add_executable(Http Main.cpp)
target_link_libraries(Http PRIVATE eacp-network)

set(BUNDLE_ID "com.eacp.httprequest")

set_target_properties(Http PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Http Request App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})