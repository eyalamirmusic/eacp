add_executable(BasicHttp Main.cpp)
target_link_libraries(BasicHttp PRIVATE eacp-network)

set(BUNDLE_ID "com.eacp.basichttprequest")

set_target_properties(BasicHttp PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Basic Http Request App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})