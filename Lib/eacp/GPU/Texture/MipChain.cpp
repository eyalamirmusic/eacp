#include "MipChain.h"

#include "../Codegen/PackedVertex.h"

#include <cstring>

namespace eacp::GPU
{
namespace
{
// The four source texels that average into one destination texel. At an odd
// extent the chain has already halved down past the last pair, so the second
// row or column clamps back onto the first and the block is a 2x1, 1x2 or 1x1 -
// which is the same rule both APIs' own generators use.
//
// Named for what it holds rather than just `Block`, and that is not a style
// preference: eacp builds as a unity build by default, so every .cpp in a
// target is concatenated into one translation unit and an anonymous namespace
// does not separate this file from ShaderGraph.cpp, which has a `Block` of its
// own. The collision is invisible in the non-unity build CLAUDE.md tells you to
// configure locally, and is a compile error in CI.
struct TexelBlock
{
    int x0, x1, y0, y1;
};

TexelBlock texelBlockFor(int x, int y, int sourceWidth, int sourceHeight)
{
    const auto x0 = x * 2;
    const auto y0 = y * 2;

    return {x0,
            x0 + 1 < sourceWidth ? x0 + 1 : x0,
            y0,
            y0 + 1 < sourceHeight ? y0 + 1 : y0};
}

// Byte channels - R8, RG8, RGBA8 and BGRA8 alike. The channel order does not
// matter to an average, which is why one function covers both RGBA and BGRA:
// averaging four texels channel by channel gives the same answer whichever
// order those channels are stored in.
void halveBytes(const std::uint8_t* source,
                std::size_t sourcePitch,
                int sourceWidth,
                int sourceHeight,
                std::uint8_t* destination,
                int destinationWidth,
                int destinationHeight,
                int channels)
{
    for (auto y = 0; y < destinationHeight; ++y)
    {
        for (auto x = 0; x < destinationWidth; ++x)
        {
            const auto block = texelBlockFor(x, y, sourceWidth, sourceHeight);

            for (auto channel = 0; channel < channels; ++channel)
            {
                const auto at = [&](int sx, int sy)
                {
                    return (int) source[(std::size_t) sy * sourcePitch
                                        + (std::size_t) (sx * channels + channel)];
                };

                const auto sum = at(block.x0, block.y0) + at(block.x1, block.y0)
                                 + at(block.x0, block.y1) + at(block.x1, block.y1);

                // +2 before the shift rounds to nearest rather than always
                // down, which over a ten-level chain is the difference between
                // a mip that holds its brightness and one that drifts dark.
                destination[(std::size_t) y
                                * (std::size_t) (destinationWidth * channels)
                            + (std::size_t) (x * channels + channel)] =
                    (std::uint8_t) ((sum + 2) / 4);
            }
        }
    }
}

void halveFloats(const std::uint8_t* source,
                 std::size_t sourcePitch,
                 int sourceWidth,
                 int sourceHeight,
                 std::uint8_t* destination,
                 int destinationWidth,
                 int destinationHeight,
                 int channels)
{
    for (auto y = 0; y < destinationHeight; ++y)
        for (auto x = 0; x < destinationWidth; ++x)
        {
            const auto block = texelBlockFor(x, y, sourceWidth, sourceHeight);

            for (auto channel = 0; channel < channels; ++channel)
            {
                const auto at = [&](int sx, int sy)
                {
                    const auto* row = source + (std::size_t) sy * sourcePitch;
                    const auto* texel = reinterpret_cast<const float*>(row);
                    return texel[sx * channels + channel];
                };

                auto* row = destination
                            + (std::size_t) y
                                  * (std::size_t) (destinationWidth * channels)
                                  * sizeof(float);

                reinterpret_cast<float*>(row)[x * channels + channel] =
                    (at(block.x0, block.y0) + at(block.x1, block.y0)
                     + at(block.x0, block.y1) + at(block.x1, block.y1))
                    * 0.25f;
            }
        }
}

// Halves through float and stores back as bits. Averaging the raw 16-bit
// patterns would be meaningless - they are a sign, an exponent and a mantissa,
// not a number - which is the trap this exists to avoid.
void halveHalves(const std::uint8_t* source,
                 std::size_t sourcePitch,
                 int sourceWidth,
                 int sourceHeight,
                 std::uint8_t* destination,
                 int destinationWidth,
                 int destinationHeight)
{
    constexpr auto channels = 4;

    for (auto y = 0; y < destinationHeight; ++y)
        for (auto x = 0; x < destinationWidth; ++x)
        {
            const auto block = texelBlockFor(x, y, sourceWidth, sourceHeight);

            for (auto channel = 0; channel < channels; ++channel)
            {
                const auto at = [&](int sx, int sy)
                {
                    const auto* row = source + (std::size_t) sy * sourcePitch;
                    const auto* texel = reinterpret_cast<const std::uint16_t*>(row);
                    return halfToFloat(texel[sx * channels + channel]);
                };

                const auto average =
                    (at(block.x0, block.y0) + at(block.x1, block.y0)
                     + at(block.x0, block.y1) + at(block.x1, block.y1))
                    * 0.25f;

                auto* row = destination
                            + (std::size_t) y
                                  * (std::size_t) (destinationWidth * channels)
                                  * sizeof(std::uint16_t);

                reinterpret_cast<std::uint16_t*>(row)[x * channels + channel] =
                    halfFromFloat(average);
            }
        }
}
} // namespace

int mipExtent(int extent, int level)
{
    const auto shifted = extent >> level;
    return shifted > 1 ? shifted : 1;
}

int mipLevelCount(int width, int height)
{
    if (width <= 0 || height <= 0)
        return 0;

    auto levels = 1;
    auto largest = width > height ? width : height;

    while (largest > 1)
    {
        largest /= 2;
        ++levels;
    }

    return levels;
}

MipChain buildMipChain(const void* pixels,
                       int width,
                       int height,
                       TextureFormat format,
                       std::size_t bytesPerRow)
{
    auto chain = MipChain {};

    if (pixels == nullptr || width <= 0 || height <= 0)
        return chain;

    const auto texelBytes = bytesPerPixel(format);
    const auto sourcePitch =
        bytesPerRow > 0 ? bytesPerRow : (std::size_t) (width * texelBytes);

    const auto levels = mipLevelCount(width, height);
    chain.levels.resize(levels);

    // Level 0, repacked to a tight stride so every level below it reads the
    // same way and the backends upload them all through one loop.
    auto& base = chain.levels[0];
    base.resize((int) ((std::size_t) width * (std::size_t) height
                       * (std::size_t) texelBytes));

    for (auto row = 0; row < height; ++row)
        std::memcpy(base.data()
                        + (std::size_t) row * (std::size_t) (width * texelBytes),
                    static_cast<const std::uint8_t*>(pixels)
                        + (std::size_t) row * sourcePitch,
                    (std::size_t) (width * texelBytes));

    for (auto level = 1; level < levels; ++level)
    {
        const auto sourceWidth = mipExtent(width, level - 1);
        const auto sourceHeight = mipExtent(height, level - 1);
        const auto destinationWidth = mipExtent(width, level);
        const auto destinationHeight = mipExtent(height, level);

        auto& destination = chain.levels[level];
        destination.resize(
            (int) ((std::size_t) destinationWidth * (std::size_t) destinationHeight
                   * (std::size_t) texelBytes));

        const auto* source = chain.levels[level - 1].data();
        const auto pitch = (std::size_t) (sourceWidth * texelBytes);

        switch (format)
        {
            case TextureFormat::R8Unorm:
            case TextureFormat::RG8Unorm:
            case TextureFormat::RGBA8Unorm:
            case TextureFormat::BGRA8Unorm:
                halveBytes(source,
                           pitch,
                           sourceWidth,
                           sourceHeight,
                           destination.data(),
                           destinationWidth,
                           destinationHeight,
                           texelBytes);
                break;

            case TextureFormat::RGBA16Float:
                halveHalves(source,
                            pitch,
                            sourceWidth,
                            sourceHeight,
                            destination.data(),
                            destinationWidth,
                            destinationHeight);
                break;

            case TextureFormat::RGBA32Float:
            case TextureFormat::R32Float:
                halveFloats(source,
                            pitch,
                            sourceWidth,
                            sourceHeight,
                            destination.data(),
                            destinationWidth,
                            destinationHeight,
                            texelBytes / (int) sizeof(float));
                break;
        }
    }

    return chain;
}
} // namespace eacp::GPU
