add_executable(KeyInspector Main.cpp)
target_link_libraries(KeyInspector PRIVATE eacp-graphics)

set(BUNDLE_ID "com.eacp.keyinspector")

set_target_properties(KeyInspector PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Key Inspector"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(KeyInspector)
set_default_target_setting(KeyInspector)
