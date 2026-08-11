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
// which of them. Stops rather than a resolved ramp, GradientRamps sharing one
// baked row per distinct stop list.
struct Gradient
{
    enum class Kind
    {
        // Runs from `start` to `end`, a fragment's place being its projection
        // onto that axis.
        Linear,

        // Runs from `start` outwards over `radius`. SVG's focal point is not
        // expressed: such a gradient draws as a concentric one.
        Radial
    };

    Kind kind = Kind::Linear;

    Point start;
    Point end;
    float radius = 0.f;

    // Applied after the placement above; SVG's gradientTransform.
    GPUWidgets::AffineTransform transform;

    GradientSpread spread = GradientSpread::Pad;

    // Need not be sorted or within 0..1: the baking sorts and clamps.
    Vector<GradientStop> stops;

    bool isEmpty() const { return stops.empty(); }
};

// A gradient resolved against the ramp texture and the space it is drawn in:
// what the shaders take, and an allocation-free value Graphics can stack.
struct GradientFill
{
    enum class Kind
    {
        // The shape is its flat colour, and the ramp is not read.
        None,
        Linear,
        Radial
    };

    Kind kind = Kind::None;

    // Maps a fragment into the gradient's own space - the segment (0,0)-(1,0)
    // for linear, the unit circle for radial. A full affine, an axis and radius
    // being wrong under skew or non-uniform scale.
    GPUWidgets::AffineTransform toGradientSpace;

    GradientSpread spread = GradientSpread::Pad;

    // The v to sample the ramp texture at; negative when there is no gradient.
    float rampV = -1.f;

    bool isEmpty() const { return kind == Kind::None; }
};
} // namespace eacp::UI
