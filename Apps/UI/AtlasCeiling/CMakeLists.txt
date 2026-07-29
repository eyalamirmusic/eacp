add_executable(AtlasCeiling Main.cpp)
target_link_libraries(AtlasCeiling PRIVATE eacp-ui)

set(BUNDLE_ID "com.eacp.atlasceiling")

set_target_properties(AtlasCeiling PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "AtlasCeiling"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(AtlasCeiling)
set_default_target_setting(AtlasCeiling)
