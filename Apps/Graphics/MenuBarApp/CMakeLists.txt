add_executable(MenuBarApp Main.cpp)
target_link_libraries(MenuBarApp PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.menubarapp")

set_target_properties(MenuBarApp PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Menu Bar App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(MenuBarApp)
set_default_target_setting(MenuBarApp)
