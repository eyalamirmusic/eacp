add_executable(Images Main.cpp)
target_link_libraries(Images PRIVATE eacp-ui)

set(BUNDLE_ID "com.eacp.images")

set_target_properties(Images PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Images"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Images)
set_default_target_setting(Images)
