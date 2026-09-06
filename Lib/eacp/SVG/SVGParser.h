#pragma once

#include "SVGBuilder.h"

namespace eacp::SVG
{
// Markup to a tree of native shape and text layers, which is the tier that
// stands on the platform's own 2D drawing stack. Declared wherever the module
// builds and defined only where EACP_HAS_CONTEXT is on, so on Linux this is a
// declaration with nothing behind it: parse the markup with parseXML and hand
// the element tree to SVGComponent::setDocument instead, which draws the same
// document through the GPU coverage rasterizer.
ParseResult parse(const std::string& svgMarkup);

} // namespace eacp::SVG
