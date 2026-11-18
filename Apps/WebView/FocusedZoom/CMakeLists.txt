add_executable(FocusedZoom Main.cpp)
target_link_libraries(FocusedZoom PRIVATE eacp-webview)

set(BUNDLE_ID "com.eacp.focusedzoom")

set_target_properties(FocusedZoom PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Focused Zoom"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(FocusedZoom)
set_default_target_setting(FocusedZoom)
