add_executable(IpcCameraDemo Main.cpp)
target_link_libraries(IpcCameraDemo PRIVATE eacp-camera-view eacp-network)

set(BUNDLE_ID "com.eacp.ipccamdemo")

set_target_properties(IpcCameraDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "IPC Camera Demo"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(IpcCameraDemo)
set_default_target_setting(IpcCameraDemo)

# macOS grants the camera (TCC) only to a bundle that says why it wants it.
eacp_add_plist_entries(IpcCameraDemo NSCameraUsageDescription
        "This demo captures the camera in one process and broadcasts the feed to a second process over eacp IPC.")
