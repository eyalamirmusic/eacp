add_executable(TrayApp Main.cpp)
target_link_libraries(TrayApp PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.trayapp")

set_target_properties(TrayApp PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Tray App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

# A menu-bar-only app: LSUIElement in this plist makes macOS launch it as an
# accessory (no Dock icon) without a flash before setDockIconVisible(false).
set_target_properties(TrayApp PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")

set_default_target_setting(TrayApp)
