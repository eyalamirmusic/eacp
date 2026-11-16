add_executable(BrowserOptions Main.cpp)
target_link_libraries(BrowserOptions PRIVATE eacp-webview)

set(BUNDLE_ID "com.eacp.browseroptions")

set_target_properties(BrowserOptions PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Browser Options"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(BrowserOptions)
set_default_target_setting(BrowserOptions)
