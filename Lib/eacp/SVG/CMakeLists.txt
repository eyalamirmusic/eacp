add_library(eacp-svg STATIC)

target_sources(eacp-svg PRIVATE
        SVGAttributes.cpp
        SVGBuilder.cpp
        SVGParser.cpp
        SVGPathParser.cpp
        XMLParser.cpp)

target_link_libraries(eacp-svg PUBLIC eacp-graphics)

set_default_target_setting(eacp-svg)
eacp_enable_unity_build(eacp-svg)
add_ide_sources(eacp-svg)
