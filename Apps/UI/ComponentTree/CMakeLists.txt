add_executable(ComponentTree Main.cpp)
target_link_libraries(ComponentTree PRIVATE eacp-ui)

set(BUNDLE_ID "com.eacp.componenttree")

set_target_properties(ComponentTree PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "ComponentTree"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(ComponentTree)
set_default_target_setting(ComponentTree)
