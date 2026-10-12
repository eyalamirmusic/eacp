#include "Texture.h"

namespace eacp::GPU
{
TextureFormatTraits textureFormatTraits(TextureFormat format)
{
    using Kind = TextureFormatKind;

    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
        case TextureFormat::BGRA8Unorm:
            return {4, 0, 4, Kind::Unorm};
        case TextureFormat::R8Unorm:
            return {1, 0, 1, Kind::Unorm};
        case TextureFormat::RG8Unorm:
            return {2, 0, 2, Kind::Unorm};
        case TextureFormat::RGBA16Float:
            return {8, 0, 4, Kind::Float};
        case TextureFormat::RGBA32Float:
            return {16, 0, 4, Kind::Float};
        case TextureFormat::R32Float:
            return {4, 0, 1, Kind::Float};
        case TextureFormat::R16Float:
            return {2, 0, 1, Kind::Float};
        case TextureFormat::RG16Float:
            return {4, 0, 2, Kind::Float};
        case TextureFormat::RG32Float:
            return {8, 0, 2, Kind::Float};
        case TextureFormat::R16Unorm:
            return {2, 0, 1, Kind::Unorm};
        case TextureFormat::R32Uint:
            return {4, 0, 1, Kind::Uint};

        case TextureFormat::RGBA8UnormSrgb:
        case TextureFormat::BGRA8UnormSrgb:
            return {4, 0, 4, Kind::Unorm, true};
        case TextureFormat::RG16Unorm:
            return {4, 0, 2, Kind::Unorm};
        case TextureFormat::RGBA16Unorm:
            return {8, 0, 4, Kind::Unorm};
        case TextureFormat::RGB10A2Unorm:
            return {4, 0, 4, Kind::Unorm};
        case TextureFormat::RG11B10Float:
        case TextureFormat::RGB9E5Float:
            return {4, 0, 3, Kind::Float};

        case TextureFormat::R8Uint:
            return {1, 0, 1, Kind::Uint};
        case TextureFormat::RG8Uint:
            return {2, 0, 2, Kind::Uint};
        case TextureFormat::RGBA8Uint:
            return {4, 0, 4, Kind::Uint};
        case TextureFormat::R16Uint:
            return {2, 0, 1, Kind::Uint};
        case TextureFormat::RG16Uint:
            return {4, 0, 2, Kind::Uint};
        case TextureFormat::RGBA16Uint:
            return {8, 0, 4, Kind::Uint};
        case TextureFormat::RG32Uint:
            return {8, 0, 2, Kind::Uint};
        case TextureFormat::RGBA32Uint:
            return {16, 0, 4, Kind::Uint};
        case TextureFormat::R8Sint:
            return {1, 0, 1, Kind::Sint};
        case TextureFormat::RG8Sint:
            return {2, 0, 2, Kind::Sint};
        case TextureFormat::RGBA8Sint:
            return {4, 0, 4, Kind::Sint};
        case TextureFormat::R16Sint:
            return {2, 0, 1, Kind::Sint};
        case TextureFormat::RG16Sint:
            return {4, 0, 2, Kind::Sint};
        case TextureFormat::RGBA16Sint:
            return {8, 0, 4, Kind::Sint};
        case TextureFormat::R32Sint:
            return {4, 0, 1, Kind::Sint};
        case TextureFormat::RG32Sint:
            return {8, 0, 2, Kind::Sint};
        case TextureFormat::RGBA32Sint:
            return {16, 0, 4, Kind::Sint};

        case TextureFormat::BC1RGBA:
            return {0, 8, 4, Kind::Unorm};
        case TextureFormat::BC2RGBA:
        case TextureFormat::BC3RGBA:
        case TextureFormat::BC7RGBA:
            return {0, 16, 4, Kind::Unorm};
        case TextureFormat::BC1RGBASrgb:
            return {0, 8, 4, Kind::Unorm, true};
        case TextureFormat::BC2RGBASrgb:
        case TextureFormat::BC3RGBASrgb:
        case TextureFormat::BC7RGBASrgb:
            return {0, 16, 4, Kind::Unorm, true};
        case TextureFormat::BC4RUnorm:
            return {0, 8, 1, Kind::Unorm};
        case TextureFormat::BC4RSnorm:
            return {0, 8, 1, Kind::Snorm};
        case TextureFormat::BC5RGUnorm:
            return {0, 16, 2, Kind::Unorm};
        case TextureFormat::BC5RGSnorm:
            return {0, 16, 2, Kind::Snorm};
        case TextureFormat::BC6HRGBUfloat:
            return {0, 16, 3, Kind::Float};
    }

    return {};
}

bool isCompressedFormat(TextureFormat format)
{
    return textureFormatTraits(format).blockBytes > 0;
}

bool isRenderableFormat(TextureFormat format)
{
    return !isCompressedFormat(format) && format != TextureFormat::RGB9E5Float;
}

int bytesPerBlock(TextureFormat format)
{
    return textureFormatTraits(format).blockBytes;
}

int bytesPerPixel(TextureFormat format)
{
    return textureFormatTraits(format).texelBytes;
}

int formatChannels(TextureFormat format)
{
    return textureFormatTraits(format).channels;
}

int levelBytesPerRow(TextureFormat format, int width)
{
    if (isCompressedFormat(format))
        return ((width + 3) / 4) * bytesPerBlock(format);

    return width * bytesPerPixel(format);
}

int levelRows(TextureFormat format, int height)
{
    return isCompressedFormat(format) ? (height + 3) / 4 : height;
}

int levelBytes(TextureFormat format, int width, int height)
{
    return levelBytesPerRow(format, width) * levelRows(format, height);
}

bool isWithin(int origin, int extent, int limit)
{
    return origin >= 0 && extent > 0 && origin + extent <= limit;
}

bool isFloatFormat(TextureFormat format)
{
    return textureFormatTraits(format).kind == TextureFormatKind::Float;
}

bool isIntegerFormat(TextureFormat format)
{
    const auto kind = textureFormatTraits(format).kind;
    return kind == TextureFormatKind::Uint || kind == TextureFormatKind::Sint;
}

bool isUnsignedIntegerFormat(TextureFormat format)
{
    return textureFormatTraits(format).kind == TextureFormatKind::Uint;
}

bool isSignedIntegerFormat(TextureFormat format)
{
    return textureFormatTraits(format).kind == TextureFormatKind::Sint;
}

bool isSrgbFormat(TextureFormat format)
{
    return textureFormatTraits(format).srgb;
}

TextureFormat srgbFormatFor(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
            return TextureFormat::RGBA8UnormSrgb;
        case TextureFormat::BGRA8Unorm:
            return TextureFormat::BGRA8UnormSrgb;
        case TextureFormat::BC1RGBA:
            return TextureFormat::BC1RGBASrgb;
        case TextureFormat::BC2RGBA:
            return TextureFormat::BC2RGBASrgb;
        case TextureFormat::BC3RGBA:
            return TextureFormat::BC3RGBASrgb;
        case TextureFormat::BC7RGBA:
            return TextureFormat::BC7RGBASrgb;
        default:
            return format;
    }
}

TextureFormat linearFormatFor(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8UnormSrgb:
            return TextureFormat::RGBA8Unorm;
        case TextureFormat::BGRA8UnormSrgb:
            return TextureFormat::BGRA8Unorm;
        case TextureFormat::BC1RGBASrgb:
            return TextureFormat::BC1RGBA;
        case TextureFormat::BC2RGBASrgb:
            return TextureFormat::BC2RGBA;
        case TextureFormat::BC3RGBASrgb:
            return TextureFormat::BC3RGBA;
        case TextureFormat::BC7RGBASrgb:
            return TextureFormat::BC7RGBA;
        default:
            return format;
    }
}

bool supportsComputeWrite(TextureFormat format)
{
    return format == TextureFormat::RGBA8Unorm
           || format == TextureFormat::RGBA16Float
           || format == TextureFormat::RGBA32Float;
}

int samplingIndex(const TextureSampling& sampling)
{
    return (sampling.filter == TextureFilter::Linear ? 2 : 0)
           + (sampling.addressMode == TextureAddressMode::Repeat ? 1 : 0);
}

TextureRegion TextureRegion::wholeLevel(int level, int slice)
{
    auto region = TextureRegion {};
    region.level = level;
    region.slice = slice;
    return region;
}
} // namespace eacp::GPU
