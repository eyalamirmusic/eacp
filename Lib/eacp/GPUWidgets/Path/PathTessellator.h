#pragma once

#include "Path.h"

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

// A vertex of an antialiased fill mesh: where it is, and how much of the shape
// reaches it. One inside the shape, zero at the outer edge of the feather, and
// interpolated across the band between them.
struct MeshVertex
{
    Graphics::Point position;
    float coverage = 1.f;
};

// The path's filled interior as a triangle list that carries its own
// antialiasing: the polygon pulled in by half of featherWidth and filled at full
// coverage, ringed by a band that fades to nothing over the other half. The
// shape's true outline is therefore the middle of the band, which is where a
// rasterized mask would have put the 50% coverage too.
//
// It exists because a mask does not scale with the shape. Coverage is computed
// per device pixel and stored, so a shape the size of a document costs the atlas
// a document's worth of texels, and a drawing of large stacked shapes runs past
// the atlas many times over -- while a mesh of it is a few hundred triangles
// whatever size it is drawn at. The trade is quality: an edge here is a linear
// ramp across a device pixel of the flattened polyline rather than the exact
// area each pixel is covered by, which a background rectangle does not need and
// a knob's indicator does.
//
// featherWidth is in path units, so a caller wanting a device pixel of ramp
// passes 1 / scale.
//
// Empty when the path cannot be filled this way, which the caller has to answer
// for by falling back to a mask:
//
//  - More than one sub-path. A hole and a second blob are the same two contours
//    here, and filling a hole solid is worse than spending the atlas. Which also
//    means the fill rule never comes up: one simple contour fills the same way
//    under either.
//  - A contour that crosses itself, which ear clipping would consume without
//    complaint and fill as the wrong shape.
//  - More points than ear clipping is worth running on, the work growing faster
//    than the count. The kernel reads segments in parallel and does not care.
//  - Ear clipping unable to consume the polygon anyway. A partial fill would be
//    a shape with a bite out of it.
Vector<MeshVertex> tessellateAntialiasedFill(const Path& path, float featherWidth);
} // namespace eacp::GPUWidgets
