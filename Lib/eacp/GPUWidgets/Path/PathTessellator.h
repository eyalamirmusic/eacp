#pragma once

#include "Path.h"

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Graphics/Primitives/Primitives.h>

namespace eacp::GPUWidgets
{
// Triangulates a path's filled interior into a triangle list: a flat array where
// every three consecutive points form one triangle, ready to upload as a vertex
// buffer. Each sub-path is filled independently as a simple polygon via ear
// clipping; self-intersecting sub-paths and holes (even-odd / non-zero fills) are
// not handled in this version.
Vector<Graphics::Point> tessellateFill(const Path& path);

// Triangulates a stroked outline of width centred on each sub-path's polyline:
// a perpendicular-offset quad per segment, with a round join between segments and
// a round cap at each end of an open sub-path (a disc at every vertex). Returns a
// flat triangle list, or empty when width <= 0. Triangles may overlap at joins, so
// a translucent stroke would double-blend - fine for the opaque strokes this
// version draws. Miter / bevel joins and butt / square caps are future work.
Vector<Graphics::Point> tessellateStroke(const Path& path, float width);
} // namespace eacp::GPUWidgets
