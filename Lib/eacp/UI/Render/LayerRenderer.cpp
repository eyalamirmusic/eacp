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

constexpr LayerVertex unitQuad[] = {
    {{0.f, 0.f}},
    {{1.f, 0.f}},
    {{1.f, 1.f}},
    {{0.f, 0.f}},
    {{1.f, 1.f}},
    {{0.f, 1.f}},
};

// Below this a pixel of the layer held no coverage at all, so there is no colour
// in it to divide back out and any answer is as good as any other. Above it the
// division is exact.
constexpr auto smallestCoverage = 1.f / 512.f;
} // namespace

struct LayerRenderer::Program final : GPU::ShaderProgram
{
    Program()
    {
        // Linear, unlike the coverage atlas beside it: a layer is drawn at the
        // size it was rendered, but its bounds are points and its texels are
        // device pixels, so the two grids need not line up to the texel the way
        // a mask's do.
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

        // The colour, weighted by the coverage the layer pass left in the alpha
        // channel. Divided out here and multiplied back by the blend, which is
        // exact wherever there is any coverage to divide by.
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
    program->setVertices(unitQuad);

    // The same blend every renderer in this tier uses, so that a layer drawn
    // into another layer accumulates coverage as correctly as one drawn onto the
    // window.
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
