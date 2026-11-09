add_executable(WebViewKeys Main.cpp)
target_link_libraries(WebViewKeys PRIVATE eacp-webview)

set(BUNDLE_ID "com.eacp.webviewkeys")

set_target_properties(WebViewKeys PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "WebView Keys"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(WebViewKeys)
set_default_target_setting(WebViewKeys)
