add_executable(AlertDemo Main.cpp)
target_link_libraries(AlertDemo PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.alertdemo")

set_target_properties(AlertDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Alert Demo"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(AlertDemo)
set_default_target_setting(AlertDemo)
