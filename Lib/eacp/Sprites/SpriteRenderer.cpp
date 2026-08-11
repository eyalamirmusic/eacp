#include "SpriteRenderer.h"

namespace eacp::Sprites
{
void SpriteShader::define()
{
    auto corner = vertexInput(&SpriteVertex::corner);

    auto origin = instanceInput(&SpriteInstance::origin, 1);
    auto edgeX = instanceInput(&SpriteInstance::edgeX, 1);
    auto edgeY = instanceInput(&SpriteInstance::edgeY, 1);
    auto uv0 = instanceInput(&SpriteInstance::uv0, 1);
    auto uv1 = instanceInput(&SpriteInstance::uv1, 1);
    auto tint = instanceInput(&SpriteInstance::tint, 1);

    auto game = origin + corner.x() * edgeX + corner.y() * edgeY;
    auto ndcX = game.x() / screenSize.x() * 2.0f - 1.0f;
    auto ndcY = 1.0f - game.y() / screenSize.y() * 2.0f;
    setPosition(float4(ndcX, ndcY, 0.0f, 1.0f));

    auto uv = uv0 + corner * (uv1 - uv0);
    setFragment(sample(image, varying(uv)) * varying(tint));
}

void Nv12Shader::define()
{
    auto corner = vertexInput(&SpriteVertex::corner);

    auto game = origin + corner.x() * edgeX + corner.y() * edgeY;
    auto ndcX = game.x() / screenSize.x() * 2.0f - 1.0f;
    auto ndcY = 1.0f - game.y() / screenSize.y() * 2.0f;
    setPosition(float4(ndcX, ndcY, 0.0f, 1.0f));

    auto uv = varying(corner);

    // Undo the coding range, then apply the track's matrix - the same
    // arithmetic Video::toImage runs, so screen and Image match.
    auto y = (sample(luma, uv).x() - yuvRange.x()) * yuvRange.y();
    auto cbcr = sample(chroma, uv);
    auto u = (cbcr.x() - yuvRange.z()) * yuvRange.w();
    auto v = (cbcr.y() - yuvRange.z()) * yuvRange.w();

    auto red = y + yuvMatrix.x() * v;
    auto green = y - yuvMatrix.y() * u - yuvMatrix.z() * v;
    auto blue = y + yuvMatrix.w() * u;

    // Coding ranges overshoot 0-1 at the extremes, and an out-of-range colour
    // would blend wrong rather than simply clip.
    auto rgb = clamp(float3(red, green, blue), 0.0f, 1.0f);

    setFragment(float4(rgb.x(), rgb.y(), rgb.z(), 1.0f) * tint);
}

namespace
{
constexpr SpriteVertex unitQuad[] = {
    {{0.0f, 0.0f}},
    {{1.0f, 0.0f}},
    {{1.0f, 1.0f}},
    {{0.0f, 0.0f}},
    {{1.0f, 1.0f}},
    {{0.0f, 1.0f}},
};

constexpr unsigned char whitePixel[] = {255, 255, 255, 255};

template <typename Program>
void prepareBlended(Program& program, int sampleCount, GPU::PixelFormat colorFormat)
{
    program.setVertices(unitQuad);
    program.prepare(sampleCount,
                    false,
                    GPU::PrimitiveTopology::Triangles,
                    GPU::BlendMode::AlphaBlend,
                    colorFormat);
}

// Lets untextured fills reuse the textured path: the fill colour is the tint,
// multiplied by white. One clamped texel reads the same through any filter, so
// sampling is immaterial.
GPU::Texture makeWhiteTexture()
{
    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = 1;
    descriptor.height = 1;
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;

    return GPU::Device::shared().makeTexture(descriptor, whitePixel);
}
// The shader divides by the logical size, and a view is zero-sized before its
// first layout, so one logical unit stands in to avoid NaNs.
Point usableSize(Point size)
{
    return {size.x > 0.0f ? size.x : 1.0f, size.y > 0.0f ? size.y : 1.0f};
}
} // namespace

SpriteRenderer::SpriteRenderer(Point logicalSizeToUse,
                               int sampleCountToUse,
                               GPU::PixelFormat colorFormatToUse)
    : logicalSize(usableSize(logicalSizeToUse))
    , sampleCount(sampleCountToUse)
    , colorFormat(colorFormatToUse)
    , white(makeWhiteTexture())
{
}

SpriteShader& SpriteRenderer::programFor(GPU::TextureSampling sampling)
{
    auto& slot = programs[GPU::samplingIndex(sampling)];

    if (!slot.has_value())
        prepareBlended(slot.emplace(sampling), sampleCount, colorFormat);

    return *slot;
}

Nv12Shader& SpriteRenderer::nv12ProgramFor(GPU::TextureSampling sampling)
{
    auto& slot = nv12Programs[GPU::samplingIndex(sampling)];

    if (!slot.has_value())
        prepareBlended(slot.emplace(sampling), sampleCount, colorFormat);

    return *slot;
}

SpriteRenderer::~SpriteRenderer()
{
    // Destroying a renderer mid-pass is already against the rules, but leave
    // anyway so the pass is not left holding a pointer to freed memory.
    detach();
}

void SpriteRenderer::begin(GPU::RenderPass& passToUse)
{
    detach();

    instances.clear();
    batchTexture = nullptr;

    pass = &passToUse;

    passToUse.addParticipant(*this);
}

void SpriteRenderer::end()
{
    flush();
    detach();
}

void SpriteRenderer::flushInto(GPU::RenderPass&)
{
    flush();

    // Not detach(): the pass is walking its participant list and drops every
    // one of them itself once the walk is over.
    pass = nullptr;
}

void SpriteRenderer::detach()
{
    if (pass == nullptr)
        return;

    pass->removeParticipant(*this);
    pass = nullptr;
}

void SpriteRenderer::setLogicalSize(Point size)
{
    const auto usable = usableSize(size);

    if (usable.x == logicalSize.x && usable.y == logicalSize.y)
        return;

    // The queued quads were issued in the old space, so they have to be drawn
    // in it.
    flush();
    logicalSize = usable;
}

void SpriteRenderer::setScissorRect(const Rect& rectInPixels)
{
    flush();

    if (pass != nullptr)
        pass->setScissorRect(rectInPixels);
}

void SpriteRenderer::clearScissorRect()
{
    flush();

    if (pass != nullptr)
        pass->clearScissorRect();
}

void SpriteRenderer::flush()
{
    if (instances.size() == 0)
        return;

    if (pass == nullptr || batchTexture == nullptr)
    {
        instances.clear();
        return;
    }

    auto& shader = programFor(batchSampling);

    shader.screenSize = Array {logicalSize.x, logicalSize.y};
    shader.image = *batchTexture;
    shader.setInstances(1, instances.data(), instances.size());

    pass->drawInstanced(shader, instances.size());

    instances.clear();
    batchTexture = nullptr;
}

void SpriteRenderer::addQuad(const GPU::Texture& texture,
                             Point origin,
                             Point edgeX,
                             Point edgeY,
                             float u0,
                             float v0,
                             float u1,
                             float v1,
                             const Color& tint,
                             GPU::TextureSampling sampling)
{
    const auto joinsRun =
        batchTexture == &texture
        && GPU::samplingIndex(batchSampling) == GPU::samplingIndex(sampling);

    if (!joinsRun)
        flush();

    batchTexture = &texture;
    batchSampling = sampling;

    auto& instance = instances.create();

    instance.origin[0] = origin.x;
    instance.origin[1] = origin.y;

    instance.edgeX[0] = edgeX.x;
    instance.edgeX[1] = edgeX.y;

    instance.edgeY[0] = edgeY.x;
    instance.edgeY[1] = edgeY.y;

    instance.uv0[0] = u0;
    instance.uv0[1] = v0;

    instance.uv1[0] = u1;
    instance.uv1[1] = v1;

    instance.tint[0] = tint.r;
    instance.tint[1] = tint.g;
    instance.tint[2] = tint.b;
    instance.tint[3] = tint.a;
}

void SpriteRenderer::drawTexture(const GPU::Texture& texture,
                                 const Rect& dst,
                                 bool flipX,
                                 bool flipY,
                                 const Color& tint,
                                 GPU::TextureSampling sampling)
{
    addQuad(texture,
            {dst.x, dst.y},
            {dst.w, 0.0f},
            {0.0f, dst.h},
            flipX ? 1.0f : 0.0f,
            flipY ? 1.0f : 0.0f,
            flipX ? 0.0f : 1.0f,
            flipY ? 0.0f : 1.0f,
            tint,
            sampling);
}

void SpriteRenderer::drawTexture(const GPU::Texture& texture,
                                 const Rect& src,
                                 const Rect& dst,
                                 const Color& tint,
                                 GPU::TextureSampling sampling)
{
    const auto width = (float) texture.width();
    const auto height = (float) texture.height();

    addQuad(texture,
            {dst.x, dst.y},
            {dst.w, 0.0f},
            {0.0f, dst.h},
            src.x / width,
            src.y / height,
            (src.x + src.w) / width,
            (src.y + src.h) / height,
            tint,
            sampling);
}

void SpriteRenderer::drawTextureQuad(const GPU::Texture& texture,
                                     Point origin,
                                     Point edgeX,
                                     Point edgeY,
                                     const Color& tint,
                                     GPU::TextureSampling sampling)
{
    addQuad(texture, origin, edgeX, edgeY, 0.0f, 0.0f, 1.0f, 1.0f, tint, sampling);
}

void SpriteRenderer::drawNv12Quad(const GPU::Texture& luma,
                                  const GPU::Texture& chroma,
                                  const YuvTransform& transform,
                                  Point origin,
                                  Point edgeX,
                                  Point edgeY,
                                  const Color& tint,
                                  GPU::TextureSampling sampling)
{
    // A video quad is its own draw, so anything queued before it must reach
    // the pass first to stay underneath.
    flush();

    if (pass == nullptr)
        return;

    auto& shader = nv12ProgramFor(sampling);

    shader.screenSize = Array {logicalSize.x, logicalSize.y};
    shader.origin = Array {origin.x, origin.y};
    shader.edgeX = Array {edgeX.x, edgeX.y};
    shader.edgeY = Array {edgeY.x, edgeY.y};
    shader.tint = Array {tint.r, tint.g, tint.b, tint.a};
    shader.yuvRange = Array {transform.lumaOffset,
                             transform.lumaScale,
                             transform.chromaOffset,
                             transform.chromaScale};
    shader.yuvMatrix =
        Array {transform.redV, transform.greenU, transform.greenV, transform.blueU};
    shader.luma = luma;
    shader.chroma = chroma;

    pass->draw(shader);
}

void SpriteRenderer::fillRect(const Rect& rect, const Color& color)
{
    addQuad(white,
            {rect.x, rect.y},
            {rect.w, 0.0f},
            {0.0f, rect.h},
            0.0f,
            0.0f,
            1.0f,
            1.0f,
            color,
            {});
}

void SpriteRenderer::drawRect(const Rect& rect, const Color& color, float thickness)
{
    const auto t = thickness;

    // Sides fit between top and bottom so corners are not double-blended.
    fillRect({rect.x, rect.y, rect.w, t}, color);
    fillRect({rect.x, rect.y + rect.h - t, rect.w, t}, color);
    fillRect({rect.x, rect.y + t, t, rect.h - 2.0f * t}, color);
    fillRect({rect.x + rect.w - t, rect.y + t, t, rect.h - 2.0f * t}, color);
}

void SpriteRenderer::drawLine(Point a, Point b, const Color& color, float thickness)
{
    const auto delta = b - a;
    const auto length = delta.length();

    if (length <= 0.0f)
        return;

    const auto half = thickness * 0.5f;

    // Segment normal scaled to the half thickness.
    const auto nx = -delta.y / length * half;
    const auto ny = delta.x / length * half;

    addQuad(white,
            {a.x - nx, a.y - ny},
            delta,
            {nx * 2.0f, ny * 2.0f},
            0.0f,
            0.0f,
            1.0f,
            1.0f,
            color,
            {});
}
} // namespace eacp::Sprites
