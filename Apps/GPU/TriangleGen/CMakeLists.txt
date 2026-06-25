add_executable(TriangleGen Main.cpp)
target_link_libraries(TriangleGen PRIVATE eacp-gpu)

set(BUNDLE_ID "com.eacp.trianglegen")

set_target_properties(TriangleGen PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "TriangleGen"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(TriangleGen)
set_default_target_setting(TriangleGen)
