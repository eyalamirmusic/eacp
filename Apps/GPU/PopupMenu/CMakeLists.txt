set(popup_menu_sources Main.cpp)

set(foreign_view_dir ${CMAKE_CURRENT_SOURCE_DIR}/../../Graphics/PopupWindow)

if (APPLE)
    list(APPEND popup_menu_sources ${foreign_view_dir}/ForeignView-macOS.mm)
elseif (WIN32)
    list(APPEND popup_menu_sources ${foreign_view_dir}/ForeignView-Windows.cpp)
endif ()

add_executable(PopupMenu ${popup_menu_sources})
target_link_libraries(PopupMenu PRIVATE eacp-gpu eacp-text)

if (APPLE)
    set_target_properties(PopupMenu PROPERTIES OBJCXX_STANDARD 20)
    target_link_libraries(PopupMenu PRIVATE "-framework AppKit")
endif ()

set(BUNDLE_ID "com.eacp.popupmenu")

set_target_properties(PopupMenu PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Popup Menu"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(PopupMenu)
set_default_target_setting(PopupMenu)
