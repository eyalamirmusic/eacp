#include "ImageBatch.h"

#include "ClipShader.h"

#include <algorithm>

namespace eacp::UI
{
namespace
{
constexpr ImageVertex imageUnitQuad[] = {
    {{0.f, 0.f}},
    {{1.f, 0.f}},
    {{1.f, 1.f}},
    {{0.f, 0.f}},
    {{1.f, 1.f}},
    {{0.f, 1.f}},
};
} // namespace

struct ImageBatch::Program final : GPU::ShaderProgram
{
    Program()
    {
        // Linear, and through the chain: an image is drawn at the size its
        // layout gave it and not at its own, so the texel grid never lines up
        // with the pixel grid the way a mask's does.
        image.sampling = {GPU::TextureFilter::Linear,
                          GPU::TextureAddressMode::Clamp};

        clipAtlas.sampling = {GPU::TextureFilter::Nearest,
                              GPU::TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto corner = vertexInput(&ImageVertex::corner);

        auto origin = instanceInput(&ImageInstance::origin, 1);
        auto edgeX = instanceInput(&ImageInstance::edgeX, 1);
        auto edgeY = instanceInput(&ImageInstance::edgeY, 1);
        auto uv0 = instanceInput(&ImageInstance::uv0, 1);
        auto uv1 = instanceInput(&ImageInstance::uv1, 1);
        auto tint = instanceInput(&ImageInstance::tint, 1);

        auto position = origin + corner.x() * edgeX + corner.y() * edgeY;
        auto clipX = position.x() / screenSize.x() * 2.f - 1.f;
        auto clipY = 1.f - position.y() / screenSize.y() * 2.f;
        setPosition(float4(clipX, clipY, 0.f, 1.f));

        auto fragUV = varying(uv0 + corner * (uv1 - uv0));
        auto fragPosition = varying(position);
        auto fragTint = varying(tint);

        auto colour = sample(image, fragUV) * fragTint;

        // The clip's coverage, out of the atlas the shapes read theirs from:
        // an image under a shaped clip is cut by the shape, one fetch rather
        // than a pass of its own. Unclipped, it reads the opaque texel and
        // multiplies by one -- see ClipShader.
        auto coverage = clipCoverage(fragPosition, clipRegion, clipMask, clipAtlas);

        setFragment(
            float4(colour.x(), colour.y(), colour.z(), colour.w() * coverage));
    }

    GPU::Uniform<GPU::Float2> screenSize;
    GPU::Uniform<GPU::Float4> clipRegion;
    GPU::Uniform<GPU::Float4> clipMask;
    GPU::Uniform<GPU::Texture2D> image;
    GPU::Uniform<GPU::Texture2D> clipAtlas;

    EACP_SHADER(screenSize, clipRegion, clipMask, image, clipAtlas)
};

ImageBatch::ImageBatch(const CoverageAtlas& atlasToUse,
                       Point logicalSizeToUse,
                       int sampleCountToUse,
                       GPU::PixelFormat colorFormatToUse)
    : atlas(atlasToUse)
    , logicalSize(logicalSizeToUse)
    , sampleCount(sampleCountToUse)
    , colorFormat(colorFormatToUse)
{
    program.create();
    program->setVertices(imageUnitQuad);

    // The blend every renderer in the tier uses, so an image drawn into a layer
    // accumulates coverage as correctly as one drawn onto the window. An image
    // holds straight alpha -- what Graphics::Image decodes to -- which is what
    // this mode takes.
    program->prepare(sampleCount,
                     false,
                     GPU::PrimitiveTopology::Triangles,
                     GPU::BlendMode::AlphaBlendOntoTransparent,
                     colorFormat);
}

ImageBatch::~ImageBatch()
{
    detach();
}

void ImageBatch::begin(GPU::RenderPass& passToUse)
{
    detach();

    instances.clear();
    batchTexture = nullptr;
    draws = 0;
    pass = &passToUse;

    passToUse.addParticipant(*this);
}

void ImageBatch::end()
{
    flush();
    detach();
}

void ImageBatch::flushInto(GPU::RenderPass&)
{
    flush();

    // Not detach(): the pass is walking its participant list right now and drops
    // every one of them itself once the walk is over.
    pass = nullptr;
}

void ImageBatch::detach()
{
    if (pass == nullptr)
        return;

    pass->removeParticipant(*this);
    pass = nullptr;
}

void ImageBatch::setLogicalSize(Point size)
{
    logicalSize = {std::max(1.f, size.x), std::max(1.f, size.y)};
}

void ImageBatch::setScissorRect(const Rect& rectInPixels)
{
    flush();

    if (pass != nullptr)
        pass->setScissorRect(rectInPixels);
}

void ImageBatch::clearScissorRect()
{
    flush();

    if (pass != nullptr)
        pass->clearScissorRect();
}

void ImageBatch::setClipMask(const ClipMask& mask)
{
    if (sameClipMask(clip, mask))
        return;

    // Before the state changes, so what was queued is drawn under the clip it
    // was issued in.
    flush();

    clip = mask;
}

void ImageBatch::flush()
{
    if (instances.empty())
        return;

    if (pass == nullptr || batchTexture == nullptr)
    {
        instances.clear();
        batchTexture = nullptr;
        return;
    }

    program->screenSize = Array {logicalSize.x, logicalSize.y};

    packClipMask(clip,
                 atlas.getOpaqueUV(),
                 program->clipRegion.value,
                 program->clipMask.value);

    program->image = *batchTexture;
    program->clipAtlas = atlas.getTexture();
    program->setInstances(1, instances.data(), instances.size());

    pass->drawInstanced(*program, instances.size());
    ++draws;

    instances.clear();
    batchTexture = nullptr;
}

void ImageBatch::draw(const GPU::Texture& texture,
                      const Rect& destination,
                      const Rect& uv,
                      const Color& tint)
{
    if (destination.w <= 0.f || destination.h <= 0.f || tint.a <= 0.f
        || !texture.isValid())
        return;

    // A quad joins the open run when it samples the texture the run does;
    // anything else is a draw of its own, and the run so far goes first so
    // the two keep the order they were issued in.
    if (batchTexture != &texture)
        flush();

    batchTexture = &texture;

    auto& instance = instances.create();

    instance.origin[0] = destination.x;
    instance.origin[1] = destination.y;
    instance.edgeX[0] = destination.w;
    instance.edgeX[1] = 0.f;
    instance.edgeY[0] = 0.f;
    instance.edgeY[1] = destination.h;

    instance.uv0[0] = uv.x;
    instance.uv0[1] = uv.y;
    instance.uv1[0] = uv.right();
    instance.uv1[1] = uv.bottom();

    instance.tint[0] = tint.r;
    instance.tint[1] = tint.g;
    instance.tint[2] = tint.b;
    instance.tint[3] = tint.a;
}
} // namespace eacp::UI
