#include "GradientShader.h"

#include "GradientRamps.h"

namespace eacp::UI
{
GPU::Float4 gradientFill(const GPU::Float4& flat,
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
} // namespace eacp::UI
