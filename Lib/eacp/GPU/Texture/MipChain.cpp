#include "MipChain.h"

#include "../Codegen/PackedScalars.h"

#include <algorithm>
#include <cmath>
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

// 16-bit unsigned normalised channels, rounded to nearest as the bytes are.
void halveShorts(const std::uint8_t* source,
                 int sourcePitch,
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
                    const auto* row = source + sy * sourcePitch;
                    const auto* texel = reinterpret_cast<const std::uint16_t*>(row);
                    return (std::uint32_t) texel[sx * channels + channel];
                };

                const auto sum = at(block.x0, block.y0) + at(block.x1, block.y0)
                                 + at(block.x0, block.y1) + at(block.x1, block.y1);

                auto* row =
                    destination
                    + y * destinationWidth * channels * (int) sizeof(std::uint16_t);

                reinterpret_cast<std::uint16_t*>(row)[x * channels + channel] =
                    (std::uint16_t) ((sum + 2) / 4);
            }
        }
}

// Byte channels - R8, RG8, RGBA8 and BGRA8 alike. The channel order does not
// matter to an average, which is why one function covers both RGBA and BGRA:
// averaging four texels channel by channel gives the same answer whichever
// order those channels are stored in.
void halveBytes(const std::uint8_t* source,
                int sourcePitch,
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
                { return (int) source[sy * sourcePitch + sx * channels + channel]; };

                const auto sum = at(block.x0, block.y0) + at(block.x1, block.y0)
                                 + at(block.x0, block.y1) + at(block.x1, block.y1);

                // +2 before the shift rounds to nearest rather than always
                // down, which over a ten-level chain is the difference between
                // a mip that holds its brightness and one that drifts dark.
                destination[y * destinationWidth * channels + x * channels
                            + channel] = (std::uint8_t) ((sum + 2) / 4);
            }
        }
    }
}

void halveFloats(const std::uint8_t* source,
                 int sourcePitch,
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
                    const auto* row = source + sy * sourcePitch;
                    const auto* texel = reinterpret_cast<const float*>(row);
                    return texel[sx * channels + channel];
                };

                auto* row = destination
                            + y * destinationWidth * channels * (int) sizeof(float);

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
                 int sourcePitch,
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
                    const auto* row = source + sy * sourcePitch;
                    const auto* texel = reinterpret_cast<const std::uint16_t*>(row);
                    return halfToFloat(texel[sx * channels + channel]);
                };

                const auto average =
                    (at(block.x0, block.y0) + at(block.x1, block.y0)
                     + at(block.x0, block.y1) + at(block.x1, block.y1))
                    * 0.25f;

                auto* row =
                    destination
                    + y * destinationWidth * channels * (int) sizeof(std::uint16_t);

                reinterpret_cast<std::uint16_t*>(row)[x * channels + channel] =
                    halfFromFloat(average);
            }
        }
}

// sRGB bytes, averaged in light rather than in code values: decoded through
// the sRGB curve, averaged, encoded back and rounded. Alpha is linear in both
// formats and averages as halveBytes does. The colour channels are the first
// three of four in either RGBA or BGRA order.
float decodeSrgbChannel(std::uint8_t value)
{
    const auto c = (float) value / 255.f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::uint8_t encodeSrgbChannel(float linear)
{
    const auto c = linear <= 0.f ? 0.f : (linear >= 1.f ? 1.f : linear);
    const auto encoded =
        c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;

    return (std::uint8_t) std::lround(encoded * 255.f);
}

void halveSrgbBytes(const std::uint8_t* source,
                    int sourcePitch,
                    int sourceWidth,
                    int sourceHeight,
                    std::uint8_t* destination,
                    int destinationWidth,
                    int destinationHeight)
{
    float decoded[256];

    for (auto value = 0; value < 256; ++value)
        decoded[value] = decodeSrgbChannel((std::uint8_t) value);

    for (auto y = 0; y < destinationHeight; ++y)
        for (auto x = 0; x < destinationWidth; ++x)
        {
            const auto block = texelBlockFor(x, y, sourceWidth, sourceHeight);

            for (auto channel = 0; channel < 4; ++channel)
            {
                const auto at = [&](int sx, int sy)
                { return source[sy * sourcePitch + sx * 4 + channel]; };

                auto* out = destination + (y * destinationWidth + x) * 4 + channel;

                if (channel == 3)
                {
                    const auto sum = at(block.x0, block.y0) + at(block.x1, block.y0)
                                     + at(block.x0, block.y1)
                                     + at(block.x1, block.y1);
                    *out = (std::uint8_t) ((sum + 2) / 4);
                    continue;
                }

                const auto light = (decoded[at(block.x0, block.y0)]
                                    + decoded[at(block.x1, block.y0)]
                                    + decoded[at(block.x0, block.y1)]
                                    + decoded[at(block.x1, block.y1)])
                                   * 0.25f;

                *out = encodeSrgbChannel(light);
            }
        }
}

// One 32-bit word per texel, unpacked into channels of `bits` widths (lowest
// first), averaged per channel and packed back - RGB10A2's 10:10:10:2.
void halvePackedUnorm(const std::uint8_t* source,
                      int sourcePitch,
                      int sourceWidth,
                      int sourceHeight,
                      std::uint8_t* destination,
                      int destinationWidth,
                      int destinationHeight)
{
    constexpr int widths[] = {10, 10, 10, 2};

    for (auto y = 0; y < destinationHeight; ++y)
        for (auto x = 0; x < destinationWidth; ++x)
        {
            const auto block = texelBlockFor(x, y, sourceWidth, sourceHeight);

            const auto word = [&](int sx, int sy)
            {
                auto value = std::uint32_t {};
                std::memcpy(&value, source + sy * sourcePitch + sx * 4, 4);
                return value;
            };

            const std::uint32_t texels[] = {word(block.x0, block.y0),
                                            word(block.x1, block.y0),
                                            word(block.x0, block.y1),
                                            word(block.x1, block.y1)};

            auto packed = std::uint32_t {0};
            auto shift = 0;

            for (const auto width: widths)
            {
                const auto mask = (1u << width) - 1u;
                auto sum = std::uint32_t {0};

                for (const auto texel: texels)
                    sum += (texel >> shift) & mask;

                packed |= ((sum + 2) / 4) << shift;
                shift += width;
            }

            std::memcpy(destination + (y * destinationWidth + x) * 4, &packed, 4);
        }
}

// RG11B10Float's unsigned floats share a half's five-bit exponent and its bias,
// so each is a half with the sign dropped and the mantissa cut to six or five
// bits: decoded by shifting into a half, encoded by rounding a half's mantissa
// down to the width, saturating at the largest finite value rather than
// carrying into infinity.
float smallFloatToFloat(std::uint32_t bits, int mantissaBits)
{
    return halfToFloat((std::uint16_t) (bits << (10 - mantissaBits)));
}

std::uint32_t smallFloatFromFloat(float value, int mantissaBits)
{
    if (!(value > 0.f))
        return 0;

    const auto drop = 10 - mantissaBits;
    const auto half = (std::uint32_t) halfFromFloat(value) & 0x7fffu;
    const auto largest = ((0x1eu << mantissaBits) | ((1u << mantissaBits) - 1u));

    if ((half >> 10) == 0x1fu)
        return largest;

    const auto rounded = (half + (1u << (drop - 1))) >> drop;
    return rounded > largest ? largest : rounded;
}

void halvePackedFloats(const std::uint8_t* source,
                       int sourcePitch,
                       int sourceWidth,
                       int sourceHeight,
                       std::uint8_t* destination,
                       int destinationWidth,
                       int destinationHeight)
{
    constexpr int mantissas[] = {6, 6, 5};

    for (auto y = 0; y < destinationHeight; ++y)
        for (auto x = 0; x < destinationWidth; ++x)
        {
            const auto block = texelBlockFor(x, y, sourceWidth, sourceHeight);

            const auto word = [&](int sx, int sy)
            {
                auto value = std::uint32_t {};
                std::memcpy(&value, source + sy * sourcePitch + sx * 4, 4);
                return value;
            };

            const std::uint32_t texels[] = {word(block.x0, block.y0),
                                            word(block.x1, block.y0),
                                            word(block.x0, block.y1),
                                            word(block.x1, block.y1)};

            auto packed = std::uint32_t {0};
            auto shift = 0;

            for (const auto mantissa: mantissas)
            {
                const auto width = mantissa + 5;
                const auto mask = (1u << width) - 1u;
                auto sum = 0.f;

                for (const auto texel: texels)
                    sum += smallFloatToFloat((texel >> shift) & mask, mantissa);

                packed |= smallFloatFromFloat(sum * 0.25f, mantissa) << shift;
                shift += width;
            }

            std::memcpy(destination + (y * destinationWidth + x) * 4, &packed, 4);
        }
}

// RGB9E5Float: nine bits of mantissa per channel, red lowest, and one five-bit
// exponent (bias 15) at the top that all three are scaled by, with no implied
// leading one - so a value is mantissa * 2^(exponent - 24). Encoded the way
// the D3D and GL specifications write it: the exponent is the brightest
// channel's, each channel rounded to the nearest mantissa at that scale, the
// exponent raised once if the brightest rounded up past nine bits, negatives
// and NaN stored as 0 and everything clamped to the largest finite value.
float sharedExponentChannel(std::uint32_t word, int channel)
{
    const auto mantissa = (word >> (9 * channel)) & 0x1ffu;
    const auto exponent = (int) (word >> 27);

    return std::ldexp((float) mantissa, exponent - 24);
}

std::uint32_t packSharedExponent(const float (&rgb)[3])
{
    constexpr auto largest = 511.f / 512.f * 65536.f;

    float clamped[3];

    for (auto channel = 0; channel < 3; ++channel)
    {
        const auto value = rgb[channel];
        clamped[channel] = value > 0.f ? (value < largest ? value : largest) : 0.f;
    }

    const auto brightest = std::max(clamped[0], std::max(clamped[1], clamped[2]));

    if (brightest <= 0.f)
        return 0;

    auto binary = 0;
    std::frexp(brightest, &binary);

    auto exponent = std::max(0, binary + 15);

    if (std::floor(std::ldexp(brightest, 24 - exponent) + 0.5f) >= 512.f)
        ++exponent;

    auto packed = (std::uint32_t) exponent << 27;

    for (auto channel = 0; channel < 3; ++channel)
    {
        const auto mantissa =
            std::floor(std::ldexp(clamped[channel], 24 - exponent) + 0.5f);
        packed |= (std::uint32_t) std::min(mantissa, 511.f) << (9 * channel);
    }

    return packed;
}

void halveSharedExponent(const std::uint8_t* source,
                         int sourcePitch,
                         int sourceWidth,
                         int sourceHeight,
                         std::uint8_t* destination,
                         int destinationWidth,
                         int destinationHeight)
{
    for (auto y = 0; y < destinationHeight; ++y)
        for (auto x = 0; x < destinationWidth; ++x)
        {
            const auto block = texelBlockFor(x, y, sourceWidth, sourceHeight);

            const auto word = [&](int sx, int sy)
            {
                auto value = std::uint32_t {};
                std::memcpy(&value, source + sy * sourcePitch + sx * 4, 4);
                return value;
            };

            const std::uint32_t texels[] = {word(block.x0, block.y0),
                                            word(block.x1, block.y0),
                                            word(block.x0, block.y1),
                                            word(block.x1, block.y1)};

            float average[3] = {};

            for (auto channel = 0; channel < 3; ++channel)
            {
                for (const auto texel: texels)
                    average[channel] += sharedExponentChannel(texel, channel);

                average[channel] *= 0.25f;
            }

            const auto packed = packSharedExponent(average);
            std::memcpy(destination + (y * destinationWidth + x) * 4, &packed, 4);
        }
}
} // namespace

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

int mipExtent(int extent, int level)
{
    const auto shifted = extent >> level;
    return shifted > 1 ? shifted : 1;
}

int mipChainBytes(TextureFormat format, int width, int height, int levels)
{
    auto total = 0;

    for (auto level = 0; level < levels; ++level)
        total +=
            levelBytes(format, mipExtent(width, level), mipExtent(height, level));

    return total;
}

bool canBuildMipChain(TextureFormat format)
{
    return !isCompressedFormat(format) && !isIntegerFormat(format);
}

bool MipChain::isValid() const
{
    return !levels.empty();
}

int MipChain::levelCount() const
{
    return levels.size();
}

const void* MipChain::level(int index) const
{
    return levels[index].data();
}

MipChain buildMipChain(
    const void* pixels, int width, int height, TextureFormat format, int bytesPerRow)
{
    auto chain = MipChain {};

    if (pixels == nullptr || width <= 0 || height <= 0)
        return chain;

    // Nothing below here can average a 4x4 block, so a compressed format leaves
    // with an empty chain rather than with levels sized and never written. See
    // canBuildMipChain.
    if (!canBuildMipChain(format))
        return chain;

    const auto texelBytes = bytesPerPixel(format);
    const auto sourcePitch = bytesPerRow > 0 ? bytesPerRow : width * texelBytes;

    const auto levels = mipLevelCount(width, height);
    chain.levels.resize(levels);

    // Level 0, repacked to a tight stride so every level below it reads the
    // same way and the backends upload them all through one loop.
    auto& base = chain.levels[0];
    base.resize(width * height * texelBytes);

    for (auto row = 0; row < height; ++row)
        std::memcpy(base.data() + row * width * texelBytes,
                    static_cast<const std::uint8_t*>(pixels) + row * sourcePitch,
                    (std::size_t) (width * texelBytes));

    for (auto level = 1; level < levels; ++level)
    {
        const auto sourceWidth = mipExtent(width, level - 1);
        const auto sourceHeight = mipExtent(height, level - 1);
        const auto destinationWidth = mipExtent(width, level);
        const auto destinationHeight = mipExtent(height, level);

        auto& destination = chain.levels[level];
        destination.resize(destinationWidth * destinationHeight * texelBytes);

        const auto* source = chain.levels[level - 1].data();
        const auto pitch = sourceWidth * texelBytes;

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
            case TextureFormat::RG16Float:
            case TextureFormat::R16Float:
                halveHalves(source,
                            pitch,
                            sourceWidth,
                            sourceHeight,
                            destination.data(),
                            destinationWidth,
                            destinationHeight,
                            texelBytes / (int) sizeof(std::uint16_t));
                break;

            case TextureFormat::R16Unorm:
            case TextureFormat::RG16Unorm:
            case TextureFormat::RGBA16Unorm:
                halveShorts(source,
                            pitch,
                            sourceWidth,
                            sourceHeight,
                            destination.data(),
                            destinationWidth,
                            destinationHeight,
                            texelBytes / (int) sizeof(std::uint16_t));
                break;

            case TextureFormat::RGBA8UnormSrgb:
            case TextureFormat::BGRA8UnormSrgb:
                halveSrgbBytes(source,
                               pitch,
                               sourceWidth,
                               sourceHeight,
                               destination.data(),
                               destinationWidth,
                               destinationHeight);
                break;

            case TextureFormat::RGB10A2Unorm:
                halvePackedUnorm(source,
                                 pitch,
                                 sourceWidth,
                                 sourceHeight,
                                 destination.data(),
                                 destinationWidth,
                                 destinationHeight);
                break;

            case TextureFormat::RG11B10Float:
                halvePackedFloats(source,
                                  pitch,
                                  sourceWidth,
                                  sourceHeight,
                                  destination.data(),
                                  destinationWidth,
                                  destinationHeight);
                break;

            case TextureFormat::RGB9E5Float:
                halveSharedExponent(source,
                                    pitch,
                                    sourceWidth,
                                    sourceHeight,
                                    destination.data(),
                                    destinationWidth,
                                    destinationHeight);
                break;

            case TextureFormat::RGBA32Float:
            case TextureFormat::RG32Float:
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

            // Turned away above, before a single level was sized. Named here
            // rather than left to a default so that a format added without a
            // filter is a -Wswitch warning, which is what caught R32Float
            // reaching this switch with no branch to land on.
            case TextureFormat::BC1RGBA:
            case TextureFormat::BC2RGBA:
            case TextureFormat::BC3RGBA:
            case TextureFormat::BC7RGBA:
            case TextureFormat::BC1RGBASrgb:
            case TextureFormat::BC2RGBASrgb:
            case TextureFormat::BC3RGBASrgb:
            case TextureFormat::BC7RGBASrgb:
            case TextureFormat::BC4RUnorm:
            case TextureFormat::BC4RSnorm:
            case TextureFormat::BC5RGUnorm:
            case TextureFormat::BC5RGSnorm:
            case TextureFormat::BC6HRGBUfloat:
            case TextureFormat::R32Uint:
            case TextureFormat::R8Uint:
            case TextureFormat::RG8Uint:
            case TextureFormat::RGBA8Uint:
            case TextureFormat::R16Uint:
            case TextureFormat::RG16Uint:
            case TextureFormat::RGBA16Uint:
            case TextureFormat::RG32Uint:
            case TextureFormat::RGBA32Uint:
            case TextureFormat::R8Sint:
            case TextureFormat::RG8Sint:
            case TextureFormat::RGBA8Sint:
            case TextureFormat::R16Sint:
            case TextureFormat::RG16Sint:
            case TextureFormat::RGBA16Sint:
            case TextureFormat::R32Sint:
            case TextureFormat::RG32Sint:
            case TextureFormat::RGBA32Sint:
                break;
        }
    }

    return chain;
}
} // namespace eacp::GPU
