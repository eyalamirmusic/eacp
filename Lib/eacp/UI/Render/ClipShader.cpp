#include "ClipShader.h"

namespace eacp::UI
{
GPU::Float clipCoverage(const GPU::Float2& position,
                        const GPU::Float4& region,
                        const GPU::Float4& mask,
                        GPU::Uniform<GPU::Texture2D>& atlas)
{
    auto place = (position - region.xy()) * region.zw();

    // Measured from the middle rather than tested against both ends, so the
    // unclipped case -- which lands exactly on zero -- is inside by the same
    // comparison that keeps a real clip's own edges.
    auto fromCentre = abs(place - 0.5f);
    auto inside = step(fromCentre.x(), 0.5f) * step(fromCentre.y(), 0.5f);

    return inside * sample(atlas, mask.xy() + place * mask.zw()).x();
}
} // namespace eacp::UI
