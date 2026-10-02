add_executable(TouchDemo Main.cpp)
target_link_libraries(TouchDemo PRIVATE eacp-ui)

set(BUNDLE_ID "com.eacp.touchdemo")

set_target_properties(TouchDemo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Touch Demo"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(TouchDemo)
set_default_target_setting(TouchDemo)
