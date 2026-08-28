add_executable(Shadows Main.cpp)
target_link_libraries(Shadows PRIVATE eacp-ui)

set(BUNDLE_ID "com.eacp.shadows")

set_target_properties(Shadows PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Shadows"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Shadows)
set_default_target_setting(Shadows)
