add_executable(Transforms Main.cpp)
target_link_libraries(Transforms PRIVATE eacp-ui)

set(BUNDLE_ID "com.eacp.transforms")

set_target_properties(Transforms PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Transforms"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Transforms)
set_default_target_setting(Transforms)
