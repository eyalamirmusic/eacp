add_executable(AspectLock Main.cpp)
target_link_libraries(AspectLock PRIVATE eacp-ui)

set(BUNDLE_ID "com.eacp.aspectlock")

set_target_properties(AspectLock PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "AspectLock"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(AspectLock)
set_default_target_setting(AspectLock)
