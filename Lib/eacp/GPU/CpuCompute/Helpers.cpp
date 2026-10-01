#include "Helpers.h"

#include <bit>
#include <cmath>

namespace eacp::GPU::CpuCompute
{
namespace
{
std::uint32_t helperShiftedDown(std::uint32_t bits, std::uint32_t amount)
{
    return bits >> (amount & 31u);
}

std::uint32_t helperHalfSign(std::uint32_t floatBits)
{
    return (floatBits >> 16u) & 0x8000u;
}

std::uint16_t helperSubnormalHalf(std::uint32_t magnitude)
{
    auto exponent = magnitude >> 23u;
    auto mantissa = (magnitude & 0x7fffffu) | 0x800000u;
    auto shift = 126u - exponent;
    auto quotient = mantissa >> shift;
    auto remainder = mantissa & ((1u << shift) - 1u);
    auto halfway = 1u << (shift - 1u);
    auto roundsUp =
        remainder > halfway || (remainder == halfway && (quotient & 1u) != 0u);

    return static_cast<std::uint16_t>(quotient + (roundsUp ? 1u : 0u));
}

float helperErfTail(float a)
{
    auto t = 1.0f / (1.0f + 0.3275911f * a);
    return t
           * (0.254829592f
              + t
                    * (-0.284496736f
                       + t
                             * (1.421413741f
                                + t * (-1.453152027f + t * 1.061405429f))))
           * std::exp(-a * a);
}

float helperSignedByte(std::uint32_t byte)
{
    return static_cast<float>(byte ^ 0x80u) - 128.0f;
}

float helperSignedNibble(std::uint32_t nibble)
{
    return static_cast<float>(nibble ^ 0x8u) - 8.0f;
}
} // namespace

float widenHalf(std::uint16_t bits)
{
    auto sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16u;
    auto exponent = (bits >> 10u) & 0x1fu;
    auto mantissa = static_cast<std::uint32_t>(bits & 0x3ffu);

    if (exponent == 0u)
    {
        auto magnitude = static_cast<float>(mantissa) * 0x1p-24f;
        return std::bit_cast<float>(std::bit_cast<std::uint32_t>(magnitude) | sign);
    }

    if (exponent == 0x1fu)
        return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13u));

    return std::bit_cast<float>(sign | ((exponent + 112u) << 23u)
                                | (mantissa << 13u));
}

std::uint16_t narrowToHalf(float value)
{
    auto bits = std::bit_cast<std::uint32_t>(value);
    auto sign = helperHalfSign(bits);
    auto magnitude = bits & 0x7fffffffu;

    if (magnitude > 0x7f800000u)
        return static_cast<std::uint16_t>(sign | 0x7e00u
                                          | ((magnitude >> 13u) & 0x3ffu));

    // 65520 is the midpoint between 65504 and the next step up; the tie goes
    // to the even side, which is the infinity.
    if (magnitude >= 0x477ff000u)
        return static_cast<std::uint16_t>(sign | 0x7c00u);

    if (magnitude >= 0x38800000u)
    {
        auto rebiased = magnitude - 0x38000000u;
        auto rounded = rebiased + 0xfffu + ((rebiased >> 13u) & 1u);
        return static_cast<std::uint16_t>(sign | (rounded >> 13u));
    }

    if (magnitude < 0x33000000u)
        return static_cast<std::uint16_t>(sign);

    return static_cast<std::uint16_t>(sign | helperSubnormalHalf(magnitude));
}

float errorFunction(float x)
{
    auto a = std::fabs(x);
    auto e = 1.0f - helperErfTail(a);
    return a == 0.0f ? x : (x < 0.0f ? -e : e);
}

float complementaryErrorFunction(float x)
{
    auto a = std::fabs(x);
    auto e = helperErfTail(a);
    return a == 0.0f ? 1.0f : (x < 0.0f ? 2.0f - e : e);
}

float saturatingTanh(float x)
{
    return x >= 10.0f ? 1.0f : (x <= -10.0f ? -1.0f : std::tanh(x));
}

HelperFloat2 unpackHalf2(std::uint32_t bits)
{
    return {widenHalf(static_cast<std::uint16_t>(bits)),
            widenHalf(static_cast<std::uint16_t>(bits >> 16u))};
}

std::uint32_t packHalf2(HelperFloat2 values)
{
    return static_cast<std::uint32_t>(narrowToHalf(values[0]))
           | (static_cast<std::uint32_t>(narrowToHalf(values[1])) << 16u);
}

float readHalf(std::uint32_t bits, std::uint32_t parity)
{
    return widenHalf(
        static_cast<std::uint16_t>(helperShiftedDown(bits, 16u * parity)));
}

HelperFloat2 unpackBFloat16x2(std::uint32_t bits)
{
    return {std::bit_cast<float>(bits << 16u),
            std::bit_cast<float>(bits & 0xffff0000u)};
}

std::uint32_t packBFloat16x2(HelperFloat2 values)
{
    auto low = std::bit_cast<std::uint32_t>(values[0]);
    auto high = std::bit_cast<std::uint32_t>(values[1]);
    auto lowIsNaN = (low & 0x7fffffffu) > 0x7f800000u;
    auto highIsNaN = (high & 0x7fffffffu) > 0x7f800000u;
    low = lowIsNaN ? (low | 0x400000u) : (low + 0x7fffu + ((low >> 16u) & 1u));
    high = highIsNaN ? (high | 0x400000u) : (high + 0x7fffu + ((high >> 16u) & 1u));
    return (low >> 16u) | (high & 0xffff0000u);
}

float readBFloat16(std::uint32_t bits, std::uint32_t parity)
{
    return std::bit_cast<float>(helperShiftedDown(bits, 16u * parity) << 16u);
}

float readInt8(std::uint32_t bits, std::uint32_t byteIndex)
{
    return helperSignedByte(helperShiftedDown(bits, 8u * byteIndex) & 0xffu);
}

float readUInt8(std::uint32_t bits, std::uint32_t byteIndex)
{
    return static_cast<float>(helperShiftedDown(bits, 8u * byteIndex) & 0xffu);
}

HelperFloat4 unpackInt8x4(std::uint32_t bits)
{
    return {helperSignedByte(bits & 0xffu),
            helperSignedByte((bits >> 8u) & 0xffu),
            helperSignedByte((bits >> 16u) & 0xffu),
            helperSignedByte((bits >> 24u) & 0xffu)};
}

HelperFloat4 unpackUInt8x4(std::uint32_t bits)
{
    return {static_cast<float>(bits & 0xffu),
            static_cast<float>((bits >> 8u) & 0xffu),
            static_cast<float>((bits >> 16u) & 0xffu),
            static_cast<float>((bits >> 24u) & 0xffu)};
}

HelperFloat4 unpackInt4x4(std::uint32_t bits)
{
    return {helperSignedNibble(bits & 0xfu),
            helperSignedNibble((bits >> 4u) & 0xfu),
            helperSignedNibble((bits >> 8u) & 0xfu),
            helperSignedNibble((bits >> 12u) & 0xfu)};
}

HelperFloat4 unpackUInt4x4(std::uint32_t bits)
{
    return {static_cast<float>(bits & 0xfu),
            static_cast<float>((bits >> 4u) & 0xfu),
            static_cast<float>((bits >> 8u) & 0xfu),
            static_cast<float>((bits >> 12u) & 0xfu)};
}

std::uint32_t packInt8x4(HelperInt4 values)
{
    return static_cast<std::uint32_t>(values[0] & 0xff)
           | (static_cast<std::uint32_t>(values[1] & 0xff) << 8u)
           | (static_cast<std::uint32_t>(values[2] & 0xff) << 16u)
           | (static_cast<std::uint32_t>(values[3] & 0xff) << 24u);
}

std::uint32_t packUInt8x4(HelperUInt4 values)
{
    return (values[0] & 0xffu) | ((values[1] & 0xffu) << 8u)
           | ((values[2] & 0xffu) << 16u) | ((values[3] & 0xffu) << 24u);
}
} // namespace eacp::GPU::CpuCompute
