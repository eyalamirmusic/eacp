add_executable(CameraProbe Main.cpp)
target_link_libraries(CameraProbe PRIVATE eacp-camera)

set(BUNDLE_ID "com.eacp.cameraprobe")

set_target_properties(CameraProbe PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Camera Probe"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(CameraProbe)

# macOS grants the camera (TCC) only to a bundle that says why it wants it.
eacp_add_plist_entries(CameraProbe NSCameraUsageDescription
        "This example captures from the camera to demonstrate the eacp camera subsystem.")
