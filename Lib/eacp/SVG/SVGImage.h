#pragma once

#include "SVGElement.h"

#include <eacp/Graphics/Image/Image.h>

#include <string_view>

namespace eacp::SVG
{
// Straight-alpha RGBA, exactly width x height pixels, over a transparent
// background. Fitted by the document's own preserveAspectRatio, so it is
// letterboxed and centred rather than stretched.
//
// Main thread, and an invalid Image when there is no GPU device or the markup
// does not parse.
Graphics::Image renderToImage(const SVGElement& root, int width, int height);
Graphics::Image renderToImage(std::string_view markup, int width, int height);
} // namespace eacp::SVG
