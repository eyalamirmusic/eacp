#include "PackedVertex.h"

#include <algorithm>
#include <cmath>

// Portable: the packing is bit arithmetic on the CPU, identical on both
// backends by construction. What each format means once it reaches the GPU is
// the two mapping tables in RenderPipeline-Apple/-Windows, and that the two
// agree is what VertexFormatTests checks.

namespace eacp::GPU
{
namespace
{
std::int16_t toSignedNormalized(float value)
{
    // Clamped rather than wrapped: a direction that drifts a hair outside the
    // unit range through arithmetic should saturate, not flip sign.
    const auto clamped = std::clamp(value, -1.0f, 1.0f);

    // 32767 rather than 32768, so +1 and -1 are both exactly representable -
    // the convention D3D12's SNORM and Metal's Short*Normalized both read back.
    return (std::int16_t) std::lround(clamped * 32767.0f);
}

std::uint8_t toUnsignedNormalized(float value)
{
    return (std::uint8_t) std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}
} // namespace

UNorm8x4 UNorm8x4::fromFloats(float x, float y, float z, float w)
{
    return {{toUnsignedNormalized(x),
             toUnsignedNormalized(y),
             toUnsignedNormalized(z),
             toUnsignedNormalized(w)}};
}

Float16x2 Float16x2::from(float x, float y)
{
    return {{halfFromFloat(x), halfFromFloat(y)}};
}

Float16x4 Float16x4::from(float x, float y, float z, float w)
{
    return {
        {halfFromFloat(x), halfFromFloat(y), halfFromFloat(z), halfFromFloat(w)}};
}

SNorm16x2 SNorm16x2::from(float x, float y)
{
    return {{toSignedNormalized(x), toSignedNormalized(y)}};
}

SNorm16x4 SNorm16x4::from(float x, float y, float z, float w)
{
    return {{toSignedNormalized(x),
             toSignedNormalized(y),
             toSignedNormalized(z),
             toSignedNormalized(w)}};
}
} // namespace eacp::GPU
