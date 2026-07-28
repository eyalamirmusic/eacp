#pragma once

#include "Path.h"

namespace eacp::GPUWidgets
{
// How a stroke ends where its sub-path is open. Butt stops flat on the last
// point, Round finishes with a half-disc past it, Square with half the width of
// straight extension - so of the three only Butt keeps the stroke inside the
// path's own bounds.
enum class LineCap
{
    Butt,
    Round,
    Square
};

// How a stroke turns a corner. Miter carries both outer edges out to where they
// meet, which on a sharp enough corner is a long way - miterLimit is where it
// gives up and bevels instead. Round fills the corner with a disc, Bevel with
// the straight chord across it.
enum class LineJoin
{
    Miter,
    Round,
    Bevel
};

struct StrokeStyle
{
    float width = 1.f;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;

    // The longest miter allowed, as a multiple of the width. Past it the corner
    // bevels. Four is the usual default and turns the corner over at about 29
    // degrees.
    float miterLimit = 4.f;
};

// The region a stroke covers, as a path to fill under the **non-zero** rule.
//
// It is not an outline. It is the quad of every segment, the join at every
// corner and the cap at every open end, each its own closed contour, all wound
// the same way and all overlapping. Under non-zero that union *is* the stroke,
// and because coverage accumulates within a path rather than between draws
// (see PathRasterizer) the overlaps leave no seam to find. Computing a true
// offset outline would mean intersecting the pieces and unioning the result,
// which is the hard part of stroking, and this rasterizer does not need it done.
//
//   auto outline = strokeToFill(path, {.width = 3.f, .join = LineJoin::Round});
//   rasterizer.setPath(outline);          // NonZero, which is the default
//
// Filled even-odd it would come apart: every overlap between two pieces would
// read as a hole. Nothing here can enforce that, so it is said once, loudly.
//
// Corners flatter than the path's own flatness tolerance are beveled whatever
// join was asked for - on a flattened curve that is nearly all of them, and a
// bevel there is the same shape as a round join to within the error the
// flattening already allows.
//
// **A stroke wants a tighter flatness than a fill does.** The source arrives
// already flattened, so its polyline is what gets offset, and offsetting
// amplifies its error: on the outside of a bend of radius r the sagitta grows by
// (r + width/2) / r, and the inner and outer edges each carry it. Measured
// against CoreGraphics on a stroked cubic, the default tolerance gives a mean
// difference of 0.044 over the boundary and the same curve at a tenth of it
// gives 0.010. Path::setFlatness before building anything meant for stroking;
// nothing here can do it afterwards, because a flattened path no longer knows
// what it was a curve of.
Path strokeToFill(const Path& path, const StrokeStyle& style);
} // namespace eacp::GPUWidgets
