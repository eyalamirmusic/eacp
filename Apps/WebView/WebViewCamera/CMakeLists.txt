add_executable(WebViewCamera Main.cpp)
target_link_libraries(WebViewCamera PRIVATE eacp-webview)

set(BUNDLE_ID "com.eacp.webviewcamera")

set_target_properties(WebViewCamera PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "WebView Camera"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(WebViewCamera)
set_default_target_setting(WebViewCamera)

# navigator.mediaDevices.getUserMedia from the WebView needs both grants.
eacp_add_plist_entries(WebViewCamera
        NSCameraUsageDescription
        "This example uses the camera to demonstrate getUserMedia from a WebView."
        NSMicrophoneUsageDescription
        "This example uses the microphone to demonstrate getUserMedia from a WebView.")
