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

# Override the shared bundle plist with one carrying NSCameraUsageDescription,
# which macOS requires before it will grant camera access (TCC).
set_target_properties(IpcCameraDemo PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
