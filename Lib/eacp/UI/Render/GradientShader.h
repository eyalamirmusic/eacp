#pragma once

#include "GradientRamps.h"

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
inline GPU::Float4 gradientFill(const GPU::Float4& flat,
                                const GPU::Float2& position,
                                const GPU::Float4& map,
                                const GPU::Float4& ramp,
                                const GPU::Float& kind,
                                GPU::Uniform<GPU::Texture2D>& ramps)
{
    // The fragment in the gradient's own space, where a linear gradient runs
    // from 0 to 1 along x and a radial one is the unit circle. Both kinds are
    // computed and one is chosen, for the reason ShapeBatch's border and fill
    // are: two pipelines and the batch break between them cost more than the
    // arithmetic does.
    auto x = map.x() * position.x() + map.y() * position.y() + ramp.x();
    auto y = map.z() * position.x() + map.w() * position.y() + ramp.y();

    auto rawT = mix(x, length(float2(x, y)), step(1.5f, kind));

    // Pad, reflect and repeat are one row read three ways: the coordinate is
    // folded into 0..1 here, so nothing about the stored colours knows which
    // mode asked for them.
    auto spread = ramp.w();
    auto repeated = rawT - floor(rawT);
    auto reflected = 1.f - abs(1.f - mod(rawT, 2.f));

    auto t = mix(mix(clamp(rawT, 0.f, 1.f), reflected, step(0.5f, spread)),
                 repeated,
                 step(1.5f, spread));

    // The ramp's own extent is the same for every row -- only which row differs
    // -- so it is a constant here rather than two more floats on every instance.
    constexpr auto firstTexel = 0.5f / (float) GradientRamps::rampWidth;
    constexpr auto lastTexel =
        ((float) GradientRamps::rampWidth - 1.f) / (float) GradientRamps::rampWidth;

    // A shape with no gradient reads the ramp anyway and throws the answer away,
    // which is the same trade the mask fetch makes and for the same reason: a
    // branch here would be a second pipeline and a batch break between two
    // shapes differing only in how they are coloured.
    auto sampled = sample(ramps, float2(firstTexel + t * lastTexel, ramp.z()));

    return mix(flat, sampled, step(0.5f, kind));
}

// The two instance fields a gradient occupies, filled in from a resolved fill.
// `map` and `ramp` are the four-float slots the shader above reads, and the
// return is what its `kind` argument wants -- so a caller writes all three and
// has nothing left to get out of step.
//
// A negative row means the ramps had no space for this gradient. The shape is
// then drawn in its flat colour, which is a picture missing its shading rather
// than one drawn through a row belonging to somebody else.
inline float packGradient(const GradientFill& fill, float* map, float* ramp)
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
