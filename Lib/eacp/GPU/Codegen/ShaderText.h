#pragma once

#include <string>

namespace eacp::GPU::ShaderEmission
{
// std::to_string's spelling of an int, usable in a constant expression.
constexpr std::string formatDecimal(int value)
{
    auto magnitude =
        value < 0 ? -static_cast<long long>(value) : static_cast<long long>(value);
    auto digits = std::string {};

    do
    {
        digits.insert(digits.begin(), static_cast<char>('0' + magnitude % 10));
        magnitude /= 10;
    } while (magnitude > 0);

    if (value < 0)
        digits.insert(digits.begin(), '-');

    return digits;
}
} // namespace eacp::GPU::ShaderEmission
