add_executable(CubeMap Main.cpp)
target_link_libraries(CubeMap PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.cubemap")

set_target_properties(CubeMap PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "CubeMap"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(CubeMap)
set_default_target_setting(CubeMap)
