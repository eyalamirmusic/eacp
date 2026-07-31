add_library(eacp-svg STATIC)

target_sources(eacp-svg PRIVATE
        NumberReader.cpp
        SVGAttributes.cpp
        SVGBuilder.cpp
        SVGComponent.cpp
        SVGParser.cpp
        SVGPathParser.cpp
        XMLParser.cpp)

# Both render tiers: eacp-graphics for the native shape layers SVGBuilder makes,
# eacp-ui for the component tree SVGComponent makes. The parse layer below them
# is blind to which - it returns colours, numbers and paths.
target_link_libraries(eacp-svg PUBLIC eacp-graphics eacp-ui)

set_default_target_setting(eacp-svg)
eacp_enable_unity_build(eacp-svg)
add_ide_sources(eacp-svg)
