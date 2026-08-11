#pragma once

#include "Common.h"
#include "SVGElement.h"

#include <eacp/GPUWidgets/Path/Path.h>

namespace eacp::SVG
{
// The six primitives and <path>.
bool isShapeTag(const std::string& tag);

// The element's geometry in its own units, flattened to `flatness`, and empty
// for anything that is not a shape. Untransformed so that a caller stroking it
// gets a pen that scales with the drawing, and skews as it should.
GPUWidgets::Path buildGeometry(const SVGElement& element, float flatness);
} // namespace eacp::SVG
