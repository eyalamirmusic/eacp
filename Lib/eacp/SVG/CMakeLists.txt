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

# The native tier of the module, and the only part of it that needs the 2D
# drawing stack: SVGBuilder makes a Graphics::ShapeLayer per shape, a
# Graphics::TextLayer per text run and a Graphics::Font to measure with, and
# SVGParser is the one-line front door that calls it. Everything above is the
# component tier, which draws through eacp-ui and so builds wherever
# EACP_HAS_TEXT does - Linux included, where these two are absent and
# SVG::parse is a declaration with no definition behind it (see SVGParser.h).
if (EACP_HAS_CONTEXT)
    target_sources(eacp-svg PRIVATE
            SVGBuilder.cpp
            SVGParser.cpp)
endif ()

# Both render tiers: eacp-graphics for the native shape layers SVGBuilder makes
# and for the Color/Point/Rect/Path primitives the parse layer returns, eacp-ui
# for the component tree SVGComponent makes. The parse layer between them is
# blind to which tier will draw what it produced.
target_link_libraries(eacp-svg PUBLIC eacp-graphics eacp-ui)

set_default_target_setting(eacp-svg)
eacp_enable_unity_build(eacp-svg)
add_ide_sources(eacp-svg)
