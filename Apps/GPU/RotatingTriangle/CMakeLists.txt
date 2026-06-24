add_executable(RotatingTriangle Main.cpp)
target_link_libraries(RotatingTriangle PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.rotatingtriangle")

set_target_properties(RotatingTriangle PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "RotatingTriangle"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(RotatingTriangle)
set_default_target_setting(RotatingTriangle)
