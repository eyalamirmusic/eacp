add_executable(TrayApp Main.cpp)
target_link_libraries(TrayApp PRIVATE eacp-webview)

set(BUNDLE_ID "com.eacp.trayapp")

set_target_properties(TrayApp PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Tray App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

# A Regular dock app (the plist has no LSUIElement): the demo proves a
# NonactivatingPanel can take the keyboard over full screen without activating.
set_target_properties(TrayApp PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")

eacp_set_gui_subsystem(TrayApp)
set_default_target_setting(TrayApp)
