#pragma once

#include "../Common.h"

#include <eacp/GPUWidgets/Path/AffineTransform.h>

namespace eacp::UI
{
// How a gradient carries on past the two ends of its own ramp.
enum class GradientSpread
{
    Pad,
    Reflect,
    Repeat
};

// A gradient fill: the colours, and the geometry deciding which fragment gets
// which of them.
//
// Stops rather than a resolved ramp, because the ramp is shared. GradientRamps
// bakes one row per distinct stop list and hands every shape asking for those
// colours the same row, so a document filling fifty shapes from one gradient
// costs one row and no batch break at all -- which is the same trade the
// coverage atlas makes for masks, and for the same reason.
struct Gradient
{
    enum class Kind
    {
        // The ramp runs from `start` to `end`, and a fragment's place on it is
        // its projection onto that axis.
        Linear,

        // The ramp runs from `start` outwards, and a fragment's place on it is
        // its distance from that centre over `radius`. SVG's focal point is not
        // expressed here: a gradient with one draws as a concentric one, which
        // is a difference in the shading rather than in the shape.
        Radial
    };

    Kind kind = Kind::Linear;

    Point start;
    Point end;
    float radius = 0.f;

    // Applied to the placement above, after it. What SVG's gradientTransform is,
    // and where a document's own transform stack lands too -- a gradient inside
    // a rotated group is placed by the same matrix its geometry was, and a
    // gradient in bounding-box units by the matrix that maps that box.
    GPUWidgets::AffineTransform transform;

    GradientSpread spread = GradientSpread::Pad;

    // In the order they are read, which is the order given. Positions outside
    // 0..1 and stops out of order are the document's business; the baking
    // sorts and clamps.
    Vector<GradientStop> stops;

    bool isEmpty() const { return stops.empty(); }
};

// A gradient resolved against the ramp texture and the space it will be drawn
// in: what the shaders actually take, and what a painter's state can hold
// without owning a stop list.
//
// It is a plain value on purpose. Graphics stacks its state once per component
// in the tree, and a state carrying a Vector of stops would allocate on every
// one of them.
struct GradientFill
{
    enum class Kind
    {
        // No gradient: the shape is its flat colour, and the ramp is not read.
        None,
        Linear,
        Radial
    };

    Kind kind = Kind::None;

    // Where a fragment is, mapped into the gradient's own space: the segment
    // from (0, 0) to (1, 0) for a linear one, and the unit circle for a radial.
    // A fragment's place along the ramp is then its x, or its distance from the
    // origin, and nothing else about the placement reaches the shader.
    //
    // A whole affine rather than an axis and a radius, because those are not
    // general enough and fail quietly. A linear gradient under a skew or a
    // non-uniform scale -- which is every bounding-box gradient on a shape that
    // is not square -- has bands that are no longer perpendicular to the line
    // between its two ends, so transforming the two ends draws the wrong thing
    // and draws it plausibly. Inverting the placement is exact for both kinds
    // and costs the fragment two dot products.
    GPUWidgets::AffineTransform toGradientSpace;

    GradientSpread spread = GradientSpread::Pad;

    // Which row of the ramp texture holds the colours, as the v to sample at.
    // Negative when there is no gradient.
    float rampV = -1.f;

    bool isEmpty() const { return kind == Kind::None; }
};
} // namespace eacp::UI
