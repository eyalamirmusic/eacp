#pragma once

#include "Common.h"
#include "SVGElement.h"

#include <eacp/GPUWidgets/Path/Path.h>

namespace eacp::SVG
{
// The elements that are a shape rather than a container or a definition: the
// six primitives and <path>.
bool isShapeTag(const std::string& tag);

// The element's geometry in its own units, flattened to `flatness`. An empty
// path for anything that is not a shape, and for a shape whose numbers describe
// nothing -- a rect of no width, a circle of no radius.
//
// Untransformed on purpose. The caller maps it afterwards, which for a stroke is
// the difference between a pen that scales with the drawing and one that does
// not: stroking here and transforming the region turns a round pen into the
// ellipse a non-uniform scale should make of it, where transforming first and
// stroking after would keep it stubbornly round.
GPUWidgets::Path buildGeometry(const SVGElement& element, float flatness);
} // namespace eacp::SVG
