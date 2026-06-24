add_executable(FancyWindow Main.cpp)
target_link_libraries(FancyWindow PRIVATE eacp-webview)

set(BUNDLE_ID "com.eacp.fancywindow")

set_target_properties(FancyWindow PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Fancy Window App"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(FancyWindow)
set_default_target_setting(FancyWindow)
