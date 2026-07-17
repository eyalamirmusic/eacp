project(IpcDemoApp VERSION 1.0.0)

add_executable(IpcDemo Main.cpp)
target_link_libraries(IpcDemo PRIVATE eacp-graphics eacp-network-rpc)

set(BUNDLE_ID "com.eacp.ipcdemo")

set_target_properties(IpcDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "IPC Demo"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(IpcDemo)
set_default_target_setting(IpcDemo)
