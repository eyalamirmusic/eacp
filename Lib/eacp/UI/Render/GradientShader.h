#pragma once

#include "Gradient.h"

#include <eacp/GPU/GPU.h>

namespace eacp::UI
{
// The gradient the two shape renderers both evaluate, written once.
//
// ShapeBatch draws quads and MeshBatch draws triangles, and everything about how
// they get a fragment to shade differs -- but what a fragment does with a
// gradient once it has its position is the same arithmetic, and it is the sort
// of arithmetic that is wrong in ways nobody sees until a document reflects one
// across a shape. So it lives here rather than in both of them.
//
// `map` is the first four of the affine that takes a fragment into the
// gradient's own space and `origin` its last two; `kind` is 0 for no gradient, 1
// for linear and 2 for radial; `ramp` carries the spread mode and the row of the
// ramp texture. Returns the colour to fill with, which for kind 0 is `flat`
// unchanged.
GPU::Float4 gradientFill(const GPU::Float4& flat,
                         const GPU::Float2& position,
                         const GPU::Float4& map,
                         const GPU::Float4& ramp,
                         const GPU::Float& kind,
                         GPU::Uniform<GPU::Texture2D>& ramps);

// The two instance fields a gradient occupies, filled in from a resolved fill.
// `map` and `ramp` are the four-float slots the shader above reads, and the
// return is what its `kind` argument wants -- so a caller writes all three and
// has nothing left to get out of step.
//
// A negative row means the ramps had no space for this gradient. The shape is
// then drawn in its flat colour, which is a picture missing its shading rather
// than one drawn through a row belonging to somebody else.
constexpr float packGradient(const GradientFill& fill, float* map, float* ramp)
{
    if (fill.isEmpty() || fill.rampV < 0.f)
        return 0.f;

    const auto& toGradient = fill.toGradientSpace;

    map[0] = toGradient.a;
    map[1] = toGradient.c;
    map[2] = toGradient.b;
    map[3] = toGradient.d;

    ramp[0] = toGradient.tx;
    ramp[1] = toGradient.ty;
    ramp[2] = fill.rampV;
    ramp[3] = (float) (int) fill.spread;

    return fill.kind == GradientFill::Kind::Radial ? 2.f : 1.f;
}
} // namespace eacp::UI
