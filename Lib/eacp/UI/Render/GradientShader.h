#pragma once

#include "GradientRamps.h"

#include <eacp/GPU/GPU.h>

namespace eacp::UI
{
// Shared by ShapeBatch and MeshBatch. `map` is the first four of the affine into
// gradient space and `ramp` its last two plus the ramp row and spread mode;
// `kind` is 0 for none (returning `flat`), 1 for linear, 2 for radial.
inline GPU::Float4 gradientFill(const GPU::Float4& flat,
                                const GPU::Float2& position,
                                const GPU::Float4& map,
                                const GPU::Float4& ramp,
                                const GPU::Float& kind,
                                GPU::Uniform<GPU::Texture2D>& ramps)
{
    // Both kinds are computed and one chosen: a branch would mean a second
    // pipeline and a batch break.
    auto x = map.x() * position.x() + map.y() * position.y() + ramp.x();
    auto y = map.z() * position.x() + map.w() * position.y() + ramp.y();

    auto rawT = mix(x, length(float2(x, y)), step(1.5f, kind));

    // Pad, reflect and repeat are one row read three ways.
    auto spread = ramp.w();
    auto repeated = rawT - floor(rawT);
    auto reflected = 1.f - abs(1.f - mod(rawT, 2.f));

    auto t = mix(mix(clamp(rawT, 0.f, 1.f), reflected, step(0.5f, spread)),
                 repeated,
                 step(1.5f, spread));

    // Constant here rather than two more floats per instance, the extent being
    // the same for every row.
    constexpr auto firstTexel = 0.5f / (float) GradientRamps::rampWidth;
    constexpr auto lastTexel =
        ((float) GradientRamps::rampWidth - 1.f) / (float) GradientRamps::rampWidth;

    // A shape with no gradient reads the ramp anyway and discards the answer.
    auto sampled = sample(ramps, float2(firstTexel + t * lastTexel, ramp.z()));

    return mix(flat, sampled, step(0.5f, kind));
}

// Fills the shader's two four-float slots and returns its `kind`. A fill whose
// ramps had no room packs as no gradient, drawing in the flat colour.
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
