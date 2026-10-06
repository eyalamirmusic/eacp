eacp_add_app(SVGDocument Main.cpp DISPLAY_NAME "SVG Document")
target_link_libraries(SVGDocument PRIVATE eacp-svg eacp-ui)
set_target_properties(SVGDocument PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(SVGDocument)
set_default_target_setting(SVGDocument)
