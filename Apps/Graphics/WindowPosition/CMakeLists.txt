add_executable(WindowPosition Main.cpp)
target_link_libraries(WindowPosition PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.windowposition")

set_target_properties(WindowPosition PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Window Position"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(WindowPosition)
set_default_target_setting(WindowPosition)
