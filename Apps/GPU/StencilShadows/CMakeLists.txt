add_executable(StencilShadows Main.cpp)
target_link_libraries(StencilShadows PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.stencilshadows")

set_target_properties(StencilShadows PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "StencilShadows"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(StencilShadows)
set_default_target_setting(StencilShadows)
