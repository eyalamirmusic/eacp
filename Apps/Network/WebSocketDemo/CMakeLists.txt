add_executable(WebSocketDemo Main.cpp)
target_link_libraries(WebSocketDemo PRIVATE eacp-network)

set(BUNDLE_ID "com.eacp.websocketdemo")

set_target_properties(WebSocketDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "WebSocket Demo App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})
