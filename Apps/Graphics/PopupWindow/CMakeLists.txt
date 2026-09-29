set(popup_window_sources Main.cpp)

if (APPLE)
    list(APPEND popup_window_sources ForeignView-macOS.mm)
elseif (WIN32)
    list(APPEND popup_window_sources ForeignView-Windows.cpp)
endif ()

add_executable(PopupWindow ${popup_window_sources})
target_link_libraries(PopupWindow PRIVATE eacp-graphics)

if (APPLE)
    set_target_properties(PopupWindow PROPERTIES OBJCXX_STANDARD 20)
    target_link_libraries(PopupWindow PRIVATE "-framework AppKit")
endif ()

set(BUNDLE_ID "com.eacp.popupwindow")

set_target_properties(PopupWindow PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Popup Window"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(PopupWindow)
set_default_target_setting(PopupWindow)
