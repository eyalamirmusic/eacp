add_executable(DockedWindows Main.cpp)
target_link_libraries(DockedWindows PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.dockedwindows")

set_target_properties(DockedWindows PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Docked Windows"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(DockedWindows)
set_default_target_setting(DockedWindows)
