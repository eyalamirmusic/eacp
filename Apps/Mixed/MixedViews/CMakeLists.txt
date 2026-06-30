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

# Override the shared bundle plist with one carrying NSCameraUsageDescription,
# which macOS requires before it will grant camera access (TCC).
set_target_properties(MixedViews PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
