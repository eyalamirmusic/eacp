#pragma once

#include <string>
#include <vector>

namespace eacp::SA3Codec::Test
{
std::vector<float> loadGoldenFloats(const std::string& name, int expectedCount);

std::string goldenDataPath(const std::string& fileName);
}
