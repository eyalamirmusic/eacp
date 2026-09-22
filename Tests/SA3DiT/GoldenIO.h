#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

namespace eacp::SA3DiT::TestSupport
{
inline std::vector<float> readGoldenFloats(const std::string& path, int count)
{
    auto file = std::ifstream(path, std::ios::binary);
    auto values = std::vector<float>((std::size_t) count);

    file.read(reinterpret_cast<char*>(values.data()),
             (std::streamsize) ((std::size_t) count * sizeof(float)));

    return values;
}

inline float maxAllcloseGap(const std::vector<float>& actual,
                            const std::vector<float>& expected,
                            float absoluteTolerance,
                            float relativeTolerance)
{
    auto worst = 0.f;

    for (auto i = std::size_t {}; i < expected.size(); ++i)
    {
        auto allowed = absoluteTolerance + relativeTolerance * std::abs(expected[i]);
        auto gap = std::abs(actual[i] - expected[i]) - allowed;

        worst = std::max(worst, gap);
    }

    return worst;
}
}
