#include "WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

namespace eacp::SA3Codec
{
namespace
{
void writeLittleEndian32(std::ofstream& file, std::uint32_t value)
{
    unsigned char bytes[4] = {(unsigned char) (value & 0xFF),
                              (unsigned char) ((value >> 8) & 0xFF),
                              (unsigned char) ((value >> 16) & 0xFF),
                              (unsigned char) ((value >> 24) & 0xFF)};
    file.write(reinterpret_cast<const char*>(bytes), 4);
}

void writeLittleEndian16(std::ofstream& file, std::uint16_t value)
{
    unsigned char bytes[2] = {(unsigned char) (value & 0xFF),
                              (unsigned char) ((value >> 8) & 0xFF)};
    file.write(reinterpret_cast<const char*>(bytes), 2);
}

std::int16_t floatToInt16(float value)
{
    auto clamped = std::clamp(value, -1.f, 1.f);
    return (std::int16_t) std::lround(clamped * 32767.f);
}
}

bool writeWavFile(const std::string& path, const StereoWaveform& waveform, int sampleRate)
{
    auto sampleCount = std::min(waveform.left.size(), waveform.right.size());

    auto file = std::ofstream {path, std::ios::binary};

    if (!file.is_open())
        return false;

    constexpr auto channels = 2;
    constexpr auto bitsPerSample = 16;
    auto byteRate = (std::uint32_t) (sampleRate * channels * bitsPerSample / 8);
    auto blockAlign = (std::uint16_t) (channels * bitsPerSample / 8);
    auto dataSize = (std::uint32_t) (sampleCount * channels * (bitsPerSample / 8));

    file.write("RIFF", 4);
    writeLittleEndian32(file, 36 + dataSize);
    file.write("WAVE", 4);

    file.write("fmt ", 4);
    writeLittleEndian32(file, 16);
    writeLittleEndian16(file, 1);
    writeLittleEndian16(file, channels);
    writeLittleEndian32(file, (std::uint32_t) sampleRate);
    writeLittleEndian32(file, byteRate);
    writeLittleEndian16(file, blockAlign);
    writeLittleEndian16(file, bitsPerSample);

    file.write("data", 4);
    writeLittleEndian32(file, dataSize);

    for (auto i = std::size_t {}; i < sampleCount; ++i)
    {
        writeLittleEndian16(file, (std::uint16_t) floatToInt16(waveform.left[i]));
        writeLittleEndian16(file, (std::uint16_t) floatToInt16(waveform.right[i]));
    }

    return file.good();
}
}
