#include "LayerRenderer.h"

#include "ClipShader.h"

#include <algorithm>

namespace eacp::UI
{
namespace
{
struct LayerVertex
{
    float corner[2];
};

constexpr LayerVertex layerUnitQuad[] = {
    {{0.f, 0.f}},
    {{1.f, 0.f}},
    {{1.f, 1.f}},
    {{0.f, 0.f}},
    {{1.f, 1.f}},
    {{0.f, 1.f}},
};

// Below this a pixel held no coverage, so there is no colour to divide out.
constexpr auto smallestCoverage = 1.f / 512.f;
} // namespace

struct LayerRenderer::Program final : GPU::ShaderProgram
{
    Program()
    {
        // Linear, unlike the coverage atlas: a layer's point bounds and device
        // texels need not line up.
        content.sampling = {GPU::TextureFilter::Linear,
                            GPU::TextureAddressMode::Clamp};

        clipAtlas.sampling = {GPU::TextureFilter::Nearest,
                              GPU::TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto corner = vertexInput(&LayerVertex::corner);

        auto position = float2(destination.x() + corner.x() * destination.z(),
                               destination.y() + corner.y() * destination.w());

        auto clipX = position.x() / screenSize.x() * 2.f - 1.f;
        auto clipY = 1.f - position.y() / screenSize.y() * 2.f;
        setPosition(float4(clipX, clipY, 0.f, 1.f));

        auto fragUV = varying(float2(contentUV.x() + corner.x() * contentUV.z(),
                                     contentUV.y() + corner.y() * contentUV.w()));

        auto fragPosition = varying(position);

        auto sampled = sample(content, fragUV);

        // Un-premultiplies; the blend multiplies it back.
        auto coverage = max(sampled.w(), smallestCoverage);
        auto colour = sampled.xyz() / coverage;

        auto alpha = sampled.w() * opacity
                     * clipCoverage(fragPosition, clipRegion, clipMask, clipAtlas);

        setFragment(float4(colour.x(), colour.y(), colour.z(), alpha));
    }

    GPU::Uniform<GPU::Float2> screenSize;
    GPU::Uniform<GPU::Float4> destination;
    GPU::Uniform<GPU::Float4> contentUV;
    GPU::Uniform<GPU::Float> opacity;
    GPU::Uniform<GPU::Float4> clipRegion;
    GPU::Uniform<GPU::Float4> clipMask;
    GPU::Uniform<GPU::Texture2D> content;
    GPU::Uniform<GPU::Texture2D> clipAtlas;

    EACP_SHADER(screenSize,
                destination,
                contentUV,
                opacity,
                clipRegion,
                clipMask,
                content,
                clipAtlas)
};

LayerRenderer::LayerRenderer(Point logicalSizeToUse,
                             int sampleCountToUse,
                             GPU::PixelFormat colorFormatToUse)
    : logicalSize(logicalSizeToUse)
    , sampleCount(sampleCountToUse)
    , colorFormat(colorFormatToUse)
{
    program.create();
    program->setVertices(layerUnitQuad);

    // The same blend every renderer in this tier uses, so a layer inside another
    // layer accumulates coverage correctly.
    program->prepare(sampleCount,
                     false,
                     GPU::PrimitiveTopology::Triangles,
                     GPU::BlendMode::AlphaBlendOntoTransparent,
                     colorFormat);
}

LayerRenderer::~LayerRenderer() = default;

void LayerRenderer::setLogicalSize(Point size)
{
    logicalSize = {std::max(1.f, size.x), std::max(1.f, size.y)};
}

void LayerRenderer::draw(GPU::RenderPass& pass,
                         const Layer& layer,
                         const Rect& destination,
                         const ClipMask& clip,
                         const CoverageAtlas& atlas)
{
    if (layer.isEmpty() || destination.w <= 0.f || destination.h <= 0.f
        || layer.getOpacity() <= 0.f)
        return;

    auto uv = layer.getContentUV();

    program->screenSize = Array {logicalSize.x, logicalSize.y};
    program->destination =
        Array<float, 4> {destination.x, destination.y, destination.w, destination.h};
    program->contentUV = Array<float, 4> {uv.x, uv.y, uv.w, uv.h};
    program->opacity = layer.getOpacity();

    packClipMask(clip,
                 atlas.getOpaqueUV(),
                 program->clipRegion.value,
                 program->clipMask.value);

    program->content = layer.getTexture();
    program->clipAtlas = atlas.getTexture();

    pass.draw(*program);
}
} // namespace eacp::UI
