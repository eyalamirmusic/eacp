add_executable(GlobalHotKeyDemo Main.cpp)
target_link_libraries(GlobalHotKeyDemo PRIVATE eacp-ui)

set(BUNDLE_ID "com.eacp.globalhotkeydemo")

set_target_properties(GlobalHotKeyDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Global Hot Key Demo"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(GlobalHotKeyDemo)
set_default_target_setting(GlobalHotKeyDemo)
