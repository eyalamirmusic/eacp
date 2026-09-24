#include "Half.h"

#include <bit>
#include <cmath>
#include <limits>

namespace eacp::ML
{
namespace
{
std::uint32_t roundedHalfMantissa(std::uint32_t value, std::uint32_t shift)
{
    auto kept = value >> shift;
    auto remainder = value & ((1u << shift) - 1);
    auto halfway = 1u << (shift - 1);

    if (remainder > halfway || (remainder == halfway && (kept & 1u)))
        ++kept;

    return kept;
}
} // namespace

std::uint16_t floatToHalf(float value)
{
    auto bits = std::bit_cast<std::uint32_t>(value);
    auto sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
    auto exponent = static_cast<int>((bits >> 23) & 0xffu);
    auto mantissa = bits & 0x7fffffu;

    if (exponent == 0xff)
        return static_cast<std::uint16_t>(sign | 0x7c00u
                                          | (mantissa != 0 ? 0x200u : 0));

    auto halfExponent = exponent - 127 + 15;

    if (halfExponent >= 0x1f)
        return static_cast<std::uint16_t>(sign | 0x7c00u);

    if (halfExponent <= 0)
    {
        if (halfExponent < -10)
            return sign;

        auto shift = static_cast<std::uint32_t>(14 - halfExponent);
        return static_cast<std::uint16_t>(
            sign | roundedHalfMantissa(mantissa | 0x800000u, shift));
    }

    auto normal = (static_cast<std::uint32_t>(halfExponent) << 23) | mantissa;
    return static_cast<std::uint16_t>(sign | roundedHalfMantissa(normal, 13));
}

float halfToFloat(std::uint16_t bits)
{
    auto sign = (bits & 0x8000u) != 0 ? -1.f : 1.f;
    auto exponent = (bits >> 10) & 0x1f;
    auto mantissa = bits & 0x3ff;

    if (exponent == 0)
        return sign * std::ldexp(static_cast<float>(mantissa), -24);

    if (exponent == 0x1f)
        return mantissa != 0 ? std::numeric_limits<float>::quiet_NaN()
                             : sign * std::numeric_limits<float>::infinity();

    return sign * std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
}

void appendHalf(Vector<std::uint8_t>& bytes, float value)
{
    auto bits = floatToHalf(value);
    bytes.add(static_cast<std::uint8_t>(bits & 0xff));
    bytes.add(static_cast<std::uint8_t>(bits >> 8));
}

Vector<std::uint8_t> halfBytes(Span<const float> values)
{
    auto bytes = Vector<std::uint8_t>();
    bytes.reserve(values.size() * 2);

    for (auto value: values)
        appendHalf(bytes, value);

    return bytes;
}
} // namespace eacp::ML
