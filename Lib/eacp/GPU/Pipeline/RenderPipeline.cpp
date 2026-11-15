#include "RenderPipeline.h"

namespace eacp::GPU
{
PixelFormat pixelFormatFor(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
            return PixelFormat::RGBA8Unorm;
        case TextureFormat::BGRA8Unorm:
            return PixelFormat::BGRA8Unorm;
        case TextureFormat::R8Unorm:
            return PixelFormat::R8Unorm;
        case TextureFormat::RG8Unorm:
            return PixelFormat::RG8Unorm;
        case TextureFormat::RGBA16Float:
            return PixelFormat::RGBA16Float;
        case TextureFormat::RGBA32Float:
            return PixelFormat::RGBA32Float;
        case TextureFormat::R32Float:
            return PixelFormat::R32Float;
        case TextureFormat::R16Float:
            return PixelFormat::R16Float;
        case TextureFormat::RG16Float:
            return PixelFormat::RG16Float;
        case TextureFormat::RG32Float:
            return PixelFormat::RG32Float;
        case TextureFormat::R16Unorm:
            return PixelFormat::R16Unorm;
        case TextureFormat::R32Uint:
            return PixelFormat::R32Uint;
        case TextureFormat::RGBA8UnormSrgb:
            return PixelFormat::RGBA8UnormSrgb;
        case TextureFormat::BGRA8UnormSrgb:
            return PixelFormat::BGRA8UnormSrgb;
        case TextureFormat::RG16Unorm:
            return PixelFormat::RG16Unorm;
        case TextureFormat::RGBA16Unorm:
            return PixelFormat::RGBA16Unorm;
        case TextureFormat::RGB10A2Unorm:
            return PixelFormat::RGB10A2Unorm;
        case TextureFormat::RG11B10Float:
            return PixelFormat::RG11B10Float;
        case TextureFormat::R8Uint:
            return PixelFormat::R8Uint;
        case TextureFormat::RG8Uint:
            return PixelFormat::RG8Uint;
        case TextureFormat::RGBA8Uint:
            return PixelFormat::RGBA8Uint;
        case TextureFormat::R16Uint:
            return PixelFormat::R16Uint;
        case TextureFormat::RG16Uint:
            return PixelFormat::RG16Uint;
        case TextureFormat::RGBA16Uint:
            return PixelFormat::RGBA16Uint;
        case TextureFormat::RG32Uint:
            return PixelFormat::RG32Uint;
        case TextureFormat::RGBA32Uint:
            return PixelFormat::RGBA32Uint;
        case TextureFormat::R8Sint:
            return PixelFormat::R8Sint;
        case TextureFormat::RG8Sint:
            return PixelFormat::RG8Sint;
        case TextureFormat::RGBA8Sint:
            return PixelFormat::RGBA8Sint;
        case TextureFormat::R16Sint:
            return PixelFormat::R16Sint;
        case TextureFormat::RG16Sint:
            return PixelFormat::RG16Sint;
        case TextureFormat::RGBA16Sint:
            return PixelFormat::RGBA16Sint;
        case TextureFormat::R32Sint:
            return PixelFormat::R32Sint;
        case TextureFormat::RG32Sint:
            return PixelFormat::RG32Sint;
        case TextureFormat::RGBA32Sint:
            return PixelFormat::RGBA32Sint;

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
        case TextureFormat::RGB9E5Float:
            break;
    }

    return PixelFormat::RGBA8Unorm;
}

TextureFormat textureFormatFor(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::BGRA8Unorm:
            return TextureFormat::BGRA8Unorm;
        case PixelFormat::RGBA8Unorm:
            return TextureFormat::RGBA8Unorm;
        case PixelFormat::RGBA16Float:
            return TextureFormat::RGBA16Float;
        case PixelFormat::RGBA32Float:
            return TextureFormat::RGBA32Float;
        case PixelFormat::R32Float:
            return TextureFormat::R32Float;
        case PixelFormat::R8Unorm:
            return TextureFormat::R8Unorm;
        case PixelFormat::RG8Unorm:
            return TextureFormat::RG8Unorm;
        case PixelFormat::R16Float:
            return TextureFormat::R16Float;
        case PixelFormat::RG16Float:
            return TextureFormat::RG16Float;
        case PixelFormat::RG32Float:
            return TextureFormat::RG32Float;
        case PixelFormat::R16Unorm:
            return TextureFormat::R16Unorm;
        case PixelFormat::R32Uint:
            return TextureFormat::R32Uint;
        case PixelFormat::RGBA8UnormSrgb:
            return TextureFormat::RGBA8UnormSrgb;
        case PixelFormat::BGRA8UnormSrgb:
            return TextureFormat::BGRA8UnormSrgb;
        case PixelFormat::RG16Unorm:
            return TextureFormat::RG16Unorm;
        case PixelFormat::RGBA16Unorm:
            return TextureFormat::RGBA16Unorm;
        case PixelFormat::RGB10A2Unorm:
            return TextureFormat::RGB10A2Unorm;
        case PixelFormat::RG11B10Float:
            return TextureFormat::RG11B10Float;
        case PixelFormat::R8Uint:
            return TextureFormat::R8Uint;
        case PixelFormat::RG8Uint:
            return TextureFormat::RG8Uint;
        case PixelFormat::RGBA8Uint:
            return TextureFormat::RGBA8Uint;
        case PixelFormat::R16Uint:
            return TextureFormat::R16Uint;
        case PixelFormat::RG16Uint:
            return TextureFormat::RG16Uint;
        case PixelFormat::RGBA16Uint:
            return TextureFormat::RGBA16Uint;
        case PixelFormat::RG32Uint:
            return TextureFormat::RG32Uint;
        case PixelFormat::RGBA32Uint:
            return TextureFormat::RGBA32Uint;
        case PixelFormat::R8Sint:
            return TextureFormat::R8Sint;
        case PixelFormat::RG8Sint:
            return TextureFormat::RG8Sint;
        case PixelFormat::RGBA8Sint:
            return TextureFormat::RGBA8Sint;
        case PixelFormat::R16Sint:
            return TextureFormat::R16Sint;
        case PixelFormat::RG16Sint:
            return TextureFormat::RG16Sint;
        case PixelFormat::RGBA16Sint:
            return TextureFormat::RGBA16Sint;
        case PixelFormat::R32Sint:
            return TextureFormat::R32Sint;
        case PixelFormat::RG32Sint:
            return TextureFormat::RG32Sint;
        case PixelFormat::RGBA32Sint:
            return TextureFormat::RGBA32Sint;
    }

    return TextureFormat::RGBA8Unorm;
}

bool isIntegerFormat(PixelFormat format)
{
    return isIntegerFormat(textureFormatFor(format));
}

BlendState blendStateFor(BlendMode mode)
{
    auto state = BlendState {};

    switch (mode)
    {
        case BlendMode::None:
            return state;

        case BlendMode::AlphaBlend:
            state.enabled = true;
            state.sourceColor = BlendFactor::SourceAlpha;
            state.destinationColor = BlendFactor::OneMinusSourceAlpha;
            state.sourceAlpha = BlendFactor::SourceAlpha;
            state.destinationAlpha = BlendFactor::OneMinusSourceAlpha;
            return state;

        case BlendMode::AlphaBlendOntoTransparent:
            state.enabled = true;
            state.sourceColor = BlendFactor::SourceAlpha;
            state.destinationColor = BlendFactor::OneMinusSourceAlpha;
            state.sourceAlpha = BlendFactor::One;
            state.destinationAlpha = BlendFactor::OneMinusSourceAlpha;
            return state;

        case BlendMode::Additive:
            state.enabled = true;
            state.sourceColor = BlendFactor::SourceAlpha;
            state.destinationColor = BlendFactor::One;
            state.sourceAlpha = BlendFactor::One;
            state.destinationAlpha = BlendFactor::One;
            return state;
    }

    return state;
}

ColorWriteMask ColorWriteMask::none()
{
    return {false, false, false, false};
}

int RenderPipelineDescriptor::colorTargetCount() const
{
    return 1 + (int) extraColorTargets.size();
}

ColorTargetState RenderPipelineDescriptor::colorTarget(int index) const
{
    if (index > 0)
        return extraColorTargets[index - 1];

    return {colorFormat, blendMode, blend, colorWriteMask};
}

ColorTargetState writtenColorTarget(const RenderPipelineDescriptor& descriptor,
                                    int index,
                                    bool hasFragment)
{
    auto target = descriptor.colorTarget(index);

    if (!hasFragment)
    {
        target.writeMask = ColorWriteMask::none();
        target.blendMode = BlendMode::None;
        target.blend.reset();
    }

    return target;
}

BlendState resolvedBlend(const ColorTargetState& target)
{
    // An integer attachment has no blend stage on any API - an id or a count
    // is not a colour to weigh against another - and Metal's validation
    // refuses a pipeline that asks for one, so a preset left on is dropped
    // here rather than failing the pipeline.
    if (isIntegerFormat(target.format))
        return blendStateFor(BlendMode::None);

    return target.blend ? *target.blend : blendStateFor(target.blendMode);
}

bool DepthBias::isNone() const
{
    return constant == 0.f && slopeScale == 0.f;
}

DepthBias appliedDepthBias(const RenderPipelineDescriptor& descriptor)
{
    if (!descriptor.depth || descriptor.depthBias.isNone())
        return {};

    return descriptor.depthBias;
}
} // namespace eacp::GPU
