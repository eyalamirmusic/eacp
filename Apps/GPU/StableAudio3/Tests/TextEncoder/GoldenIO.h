#pragma once

#include <fstream>
#include <string>
#include <vector>

namespace eacp::SA3TextEncoder::Test
{
inline std::vector<float> readBinaryFloats(const std::string& path, std::size_t count)
{
    auto file = std::ifstream {path, std::ios::binary};
    auto values = std::vector<float>(count);

    file.read(reinterpret_cast<char*>(values.data()),
             (std::streamsize) (count * sizeof(float)));

    return values;
}
}
