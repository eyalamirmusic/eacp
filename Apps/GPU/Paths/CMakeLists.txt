add_executable(Paths Main.cpp)
target_link_libraries(Paths PRIVATE eacp-gpuwidgets)

set(BUNDLE_ID "com.eacp.paths")

set_target_properties(Paths PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Paths"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(Paths)
set_default_target_setting(Paths)
