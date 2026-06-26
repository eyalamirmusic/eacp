find_package(MakeASound REQUIRED)

eacp_add_webview_app(TrayApp
        SOURCES     Main.cpp
        API         Api::TrayLauncherApi
        API_HEADER  Types.h
        WEB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/web
        BUNDLE_ID   com.eacp.trayapp
        BUNDLE_NAME "Tray App"
        LINK_LIBRARIES MakeASound miniaudio
        CATEGORY    TrayApp
        REACT)

if(APPLE)
    target_sources(TrayApp PRIVATE Clipboard-macOS.mm)
elseif(WIN32)
    target_sources(TrayApp PRIVATE Clipboard-Windows.cpp)
endif()

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

eacp_set_gui_subsystem(TrayApp)
set_default_target_setting(TrayApp)
