add_executable(HttpServer Main.cpp)
target_link_libraries(HttpServer PRIVATE eacp-server)

set(BUNDLE_ID "com.eacp.httpserver")

set_target_properties(HttpServer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Http Server App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})
