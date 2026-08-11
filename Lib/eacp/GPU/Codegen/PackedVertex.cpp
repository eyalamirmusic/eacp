#include "PackedVertex.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace eacp::GPU
{
namespace
{
constexpr auto smallestNormalAsFloat = std::uint32_t {0x38800000}; // 2^-14
constexpr auto tooLargeForHalfAsFloat = std::uint32_t {0x47800000}; // 65536
constexpr auto roundsToZeroAsFloat = std::uint32_t {0x33000000}; // 2^-25
constexpr auto infinityAsFloat = std::uint32_t {0x7F800000};

// 32767 rather than 32768, so +1 and -1 are both exactly representable - the
// convention D3D12's SNORM and Metal's Short*Normalized both read back.
constexpr auto signedNormalizedScale = 32767.0f;

std::int16_t toSignedNormalized(float value)
{
    const auto clamped = std::clamp(value, -1.0f, 1.0f);
    return (std::int16_t) std::lround(clamped * signedNormalizedScale);
}

std::uint8_t toUnsignedNormalized(float value)
{
    return (std::uint8_t) std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}
} // namespace

std::uint16_t halfFromFloat(float value)
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = (std::uint16_t) ((bits >> 16) & 0x8000u);
    const auto magnitude = bits & 0x7FFFFFFFu;

    // A NaN has to stay a NaN: rounding one down would land it on infinity.
    if (magnitude >= infinityAsFloat)
        return (std::uint16_t) (sign | 0x7C00u
                                | (magnitude > infinityAsFloat ? 0x200u : 0u));

    if (magnitude >= tooLargeForHalfAsFloat)
        return (std::uint16_t) (sign | 0x7C00u);

    if (magnitude >= smallestNormalAsFloat)
    {
        // Rebias the exponent, rounding the mantissa to nearest even.
        const auto rounded = magnitude + 0x0FFFu + ((magnitude >> 13) & 1u);
        return (std::uint16_t) (sign | ((rounded - 0x38000000u) >> 13));
    }

    if (magnitude <= roundsToZeroAsFloat)
        return sign;

    // Subnormal: stored as a plain multiple of 2^-24 with no exponent of its
    // own, so scaling by 2^24 and rounding lands on that multiple directly.
    const auto scaled = std::bit_cast<float>(magnitude) * 16777216.0f;

    return (std::uint16_t) (sign | (std::uint16_t) std::lround(scaled));
}

float halfToFloat(std::uint16_t bits)
{
    const auto sign = (std::uint32_t) (bits & 0x8000u) << 16;
    const auto exponent = (std::uint32_t) ((bits >> 10) & 0x1Fu);
    const auto mantissa = (std::uint32_t) (bits & 0x3FFu);

    if (exponent == 0x1F)
        return std::bit_cast<float>(sign | infinityAsFloat | (mantissa << 13));

    if (exponent != 0)
        return std::bit_cast<float>(sign | ((exponent + 112u) << 23)
                                    | (mantissa << 13));

    if (mantissa == 0)
        return std::bit_cast<float>(sign);

    // Subnormal in half, ordinary in float: shift the leading one into the
    // implicit position and drop the exponent to match how far it moved.
    auto shift = 0u;
    auto significand = mantissa;

    while ((significand & 0x400u) == 0)
    {
        significand <<= 1;
        ++shift;
    }

    return std::bit_cast<float>(sign | ((113u - shift) << 23)
                                | ((significand & 0x3FFu) << 13));
}

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
