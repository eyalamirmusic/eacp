add_executable(SVGDocument Main.cpp)
target_link_libraries(SVGDocument PRIVATE eacp-svg eacp-ui)

set(BUNDLE_ID "com.eacp.svgdocument")

set_target_properties(SVGDocument PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "SVGDocument"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID})

eacp_set_gui_subsystem(SVGDocument)
set_default_target_setting(SVGDocument)
