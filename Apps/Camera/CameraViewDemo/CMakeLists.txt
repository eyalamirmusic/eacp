add_executable(CameraViewDemo Main.cpp)
target_link_libraries(CameraViewDemo PRIVATE eacp-camera-view)

set(BUNDLE_ID "com.eacp.cameraviewdemo")

set_target_properties(CameraViewDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Camera View Demo"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(CameraViewDemo)
set_default_target_setting(CameraViewDemo)

# macOS grants the camera (TCC) only to a bundle that says why it wants it.
eacp_add_plist_entries(CameraViewDemo NSCameraUsageDescription
        "This demo shows the live camera in a GPU view to exercise the eacp camera subsystem.")
