add_executable(CameraProbe Main.cpp)
target_link_libraries(CameraProbe PRIVATE eacp-camera)

set(BUNDLE_ID "com.eacp.cameraprobe")

set_target_properties(CameraProbe PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Camera Probe"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(CameraProbe)

# Override the shared bundle plist with one carrying NSCameraUsageDescription,
# which macOS requires before it will grant camera access (TCC).
set_target_properties(CameraProbe PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
