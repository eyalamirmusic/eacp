#pragma once

#include "SVGBuilder.h"

namespace eacp::SVG
{
// Markup to a tree of native shape and text layers. Defined only under
// EACP_HAS_CONTEXT; elsewhere use parseXML with SVGComponent::setDocument.
ParseResult parse(const std::string& svgMarkup);

} // namespace eacp::SVG
