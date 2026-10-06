eacp_add_app(CameraViewDemo Main.cpp
        BUNDLE_ID "com.eacp.cameraviewdemo"
        DISPLAY_NAME "Camera View Demo"
        PERMISSIONS CAMERA)
target_link_libraries(CameraViewDemo PRIVATE eacp-camera-view)
set_target_properties(CameraViewDemo PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(CameraViewDemo)
set_default_target_setting(CameraViewDemo)

# macOS grants the camera (TCC) only to a bundle that says why it wants it.
eacp_add_plist_entries(CameraViewDemo NSCameraUsageDescription
        "This demo shows the live camera in a GPU view to exercise the eacp camera subsystem.")
