#pragma once

#include "SVGElement.h"
#include <eacp/Graphics/Primitives/Path.h>

#include <optional>

namespace eacp::SVG
{

// Geometry for one of the SVG shape tags (rect, circle, ellipse, line,
// polyline, polygon, path), with all coordinates scaled by (sx, sy).
// nullopt for other tags and for shapes with no drawable geometry.
std::optional<Graphics::Path>
    makeShapePath(const SVGElement& element, float sx, float sy);

// Natural document size of an <svg> root: the width/height attributes,
// falling back to the viewBox and then the CSS default of 300x150.
Graphics::Point documentSize(const SVGElement& root);

} // namespace eacp::SVG
