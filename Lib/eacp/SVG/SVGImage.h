#pragma once

#include "SVGElement.h"

#include <eacp/Graphics/Graphics/GraphicsContext.h>
#include <eacp/Graphics/Image/Image.h>

namespace eacp::SVG
{

// Rasterizes SVG markup into a straight-alpha RGBA Image without needing a
// window or view. With no size given the SVG's natural size is used; giving
// one dimension preserves the aspect ratio; giving both stretches, matching
// SVGView::stretchToFit. Returns an invalid image (operator bool is false)
// when the markup does not parse or the resolved size is empty.
Graphics::Image toImage(const std::string& svgMarkup, int width = 0, int height = 0);

// Draws a parsed <svg> element tree into an existing context, with all
// coordinates scaled by (sx, sy).
void render(Graphics::Context& context, const SVGElement& root, float sx, float sy);

} // namespace eacp::SVG
