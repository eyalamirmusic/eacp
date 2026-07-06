add_executable(Instancing Main.cpp)
target_link_libraries(Instancing PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.instancing")

set_target_properties(Instancing PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Instancing"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Instancing)
set_default_target_setting(Instancing)
