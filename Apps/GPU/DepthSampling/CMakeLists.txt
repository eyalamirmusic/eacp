add_executable(DepthSampling Main.cpp)
target_link_libraries(DepthSampling PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.depthsampling")

set_target_properties(DepthSampling PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "DepthSampling"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(DepthSampling)
set_default_target_setting(DepthSampling)
