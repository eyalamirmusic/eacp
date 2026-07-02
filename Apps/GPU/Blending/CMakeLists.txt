add_executable(Blending Main.cpp)
target_link_libraries(Blending PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.blending")

set_target_properties(Blending PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Blending"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Blending)
set_default_target_setting(Blending)
