add_executable(Http Main.cpp)
target_link_libraries(Http PRIVATE eacp-core)

set_target_properties(Http PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Http Request App"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.eacp.httprequest")