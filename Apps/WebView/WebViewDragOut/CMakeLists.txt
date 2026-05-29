add_executable(WebViewDragOut Main.cpp)
target_link_libraries(WebViewDragOut PRIVATE eacp-webview)

# No package.json under web/ -> eacp_webview_add_vite embeds the prebuilt
# dist/ as-is. The same category is read back from C++ (ResEmbed::get) to
# materialise the draggable files to disk.
eacp_webview_add_vite(WebViewDragOut
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web
        NAMESPACE WebResources
        CATEGORY DragOutApp)

set(BUNDLE_ID "com.eacp.webviewdragout")

set_target_properties(WebViewDragOut PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "WebView Drag Out"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(WebViewDragOut)
