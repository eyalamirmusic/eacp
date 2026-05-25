add_executable(WebViewCamera Main.cpp)
target_link_libraries(WebViewCamera PRIVATE eacp-webview)

set(BUNDLE_ID "com.eacp.webviewcamera")

set_target_properties(WebViewCamera PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "WebView Camera"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(WebViewCamera)

# Override the shared bundle plist with one that carries the
# NSCameraUsageDescription / NSMicrophoneUsageDescription entries
# required for navigator.mediaDevices.getUserMedia from the WebView.
set_target_properties(WebViewCamera PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
