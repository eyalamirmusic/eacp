add_executable(Clipping Main.cpp)
target_link_libraries(Clipping PRIVATE eacp-sprites)

set(BUNDLE_ID "com.eacp.clipping")

set_target_properties(Clipping PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Clipping"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Clipping)
set_default_target_setting(Clipping)
