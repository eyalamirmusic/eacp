#include "GoldenFixture.h"

#include <fstream>
#include <stdexcept>

namespace eacp::SA3Codec::Test
{
std::string goldenDataPath(const std::string& fileName)
{
    return std::string {SA3_CODEC_GOLDEN_DIR} + "/" + fileName;
}

std::vector<float> loadGoldenFloats(const std::string& name, int expectedCount)
{
    auto path = goldenDataPath(name + ".bin");
    auto stream = std::ifstream {path, std::ios::binary};

    if (!stream)
        throw std::runtime_error("could not open golden fixture: " + path);

    auto values = std::vector<float> ((std::size_t) expectedCount);
    stream.read(reinterpret_cast<char*>(values.data()),
               (std::streamsize) (values.size() * sizeof(float)));

    if (!stream)
        throw std::runtime_error("golden fixture shorter than expected: " + path);

    return values;
}
}
