add_executable(CameraViewDemo Main.cpp)
target_link_libraries(CameraViewDemo PRIVATE eacp-camera-view)

set(BUNDLE_ID "com.eacp.cameraviewdemo")

set_target_properties(CameraViewDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Camera View Demo"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

set_default_target_setting(CameraViewDemo)

# Override the shared bundle plist with one carrying NSCameraUsageDescription,
# which macOS requires before it will grant camera access (TCC).
set_target_properties(CameraViewDemo PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
