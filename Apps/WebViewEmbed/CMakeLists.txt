add_executable(WebViewEmbed Main.cpp)
target_link_libraries(WebViewEmbed PRIVATE eacp-webview eacp-network)

eacp_webview_add_vite(WebViewEmbed
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
        NAMESPACE WebResources
        CATEGORY WebApp)

set(BUNDLE_ID "com.eacp.webviewembed")

set_target_properties(WebViewEmbed PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "WebView Embed"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(WebViewEmbed)
