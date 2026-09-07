add_library(eacp-svg STATIC)

target_sources(eacp-svg PRIVATE
        NumberReader.cpp
        SVGAttributes.cpp
        SVGClip.cpp
        SVGComponent.cpp
        SVGGeometry.cpp
        SVGGradient.cpp
        SVGImage.cpp
        SVGPathParser.cpp
        XMLParser.cpp)

# The native tier: SVGBuilder makes a ShapeLayer per shape and a TextLayer per
# text run, and SVGParser is the front door that calls it.
if (EACP_HAS_CONTEXT)
    target_sources(eacp-svg PRIVATE
            SVGBuilder.cpp
            SVGParser.cpp)
endif ()

# Both render tiers: eacp-graphics for the native shape layers SVGBuilder makes,
# eacp-ui for the component tree SVGComponent makes. The parse layer below them
# is blind to which - it returns colours, numbers and paths.
target_link_libraries(eacp-svg PUBLIC eacp-graphics eacp-ui)

set_default_target_setting(eacp-svg)
eacp_enable_unity_build(eacp-svg)
add_ide_sources(eacp-svg)
