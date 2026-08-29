add_executable(TextBesideWebView Main.cpp)
target_link_libraries(TextBesideWebView PRIVATE eacp-ui eacp-webview)

set(BUNDLE_ID "com.eacp.textbesidewebview")

set_target_properties(TextBesideWebView PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Text Beside WebView"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(TextBesideWebView)
set_default_target_setting(TextBesideWebView)
