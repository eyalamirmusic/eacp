#pragma once

#include "Path.h"

namespace eacp::GPUWidgets
{
// A flat triangle list - every three consecutive points are one triangle. Each
// sub-path is ear-clipped as a simple polygon: no holes, no self-intersection.
Vector<Graphics::Point> tessellateFill(const Path& path);

// A flat triangle list of a width-wide stroke centred on the polyline, round
// joins and caps, empty when width <= 0. Triangles overlap at joins, so a
// translucent stroke double-blends.
Vector<Graphics::Point> tessellateStroke(const Path& path, float width);

// coverage is 1 inside the shape and 0 at the outer edge of the feather band.
struct MeshVertex
{
    Graphics::Point position;
    float coverage = 1.f;
};

// A self-antialiasing fill mesh, its feather band straddling the true outline.
// featherWidth is in path units (1 / scale for one device pixel). Empty unless
// the path is one simple contour, leaving the caller to fall back to a mask.
Vector<MeshVertex> tessellateAntialiasedFill(const Path& path, float featherWidth);
} // namespace eacp::GPUWidgets
