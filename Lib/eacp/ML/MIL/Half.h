#pragma once

#include "../Common.h"

#include <cstdint>

namespace eacp::ML
{
// IEEE binary16 bits, rounded to nearest even: the form every fp16 tensor
// takes in the weight blob and every fp16 scalar takes inline.
std::uint16_t floatToHalf(float value);
float halfToFloat(std::uint16_t bits);

void appendHalf(Vector<std::uint8_t>& bytes, float value);
Vector<std::uint8_t> halfBytes(Span<const float> values);
} // namespace eacp::ML
