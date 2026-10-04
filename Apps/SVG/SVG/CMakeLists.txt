eacp_add_app(SVG Main.cpp)
target_link_libraries(SVG PRIVATE eacp-svg)

find_package(ResEmbed REQUIRED)
res_embed_add(SVG
        FILES example.svg
        NAMESPACE SVGAssets
        CATEGORY SVGAssets)

set_target_properties(SVG PROPERTIES MACOSX_BUNDLE TRUE)

eacp_set_gui_subsystem(SVG)
set_default_target_setting(SVG)
