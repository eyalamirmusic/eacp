add_executable(MixedViews Main.cpp)
target_link_libraries(MixedViews PRIVATE eacp-camera-view eacp-webview)

set(BUNDLE_ID "com.eacp.mixedviews")

set_target_properties(MixedViews PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Mixed Views"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(MixedViews)
set_default_target_setting(MixedViews)

# macOS grants the camera (TCC) only to a bundle that says why it wants it.
eacp_add_plist_entries(MixedViews NSCameraUsageDescription
        "This demo shows the live camera under a transparent WebView whose sliders drive the native GPU camera view.")
