#include "SpriteRenderer.h"

namespace eacp::Sprites
{
void SpriteShader::define()
{
    auto corner = vertexInput(&SpriteVertex::corner);

    auto game = origin + corner.x() * edgeX + corner.y() * edgeY;
    auto ndcX = game.x() / screenSize.x() * 2.0f - 1.0f;
    auto ndcY = 1.0f - game.y() / screenSize.y() * 2.0f;
    setPosition(float4(ndcX, ndcY, 0.0f, 1.0f));

    auto uv = uv0 + corner * (uv1 - uv0);
    setFragment(sample(image, varying(uv)) * tint);
}

void Nv12Shader::define()
{
    auto corner = vertexInput(&SpriteVertex::corner);

    auto game = origin + corner.x() * edgeX + corner.y() * edgeY;
    auto ndcX = game.x() / screenSize.x() * 2.0f - 1.0f;
    auto ndcY = 1.0f - game.y() / screenSize.y() * 2.0f;
    setPosition(float4(ndcX, ndcY, 0.0f, 1.0f));

    auto uv = varying(corner);

    // Undo the coding range, then apply the track's matrix. Video::toImage runs
    // the same arithmetic from the same constants, so a frame looks identical
    // whether it reached the screen or an Image.
    auto y = (sample(luma, uv).x() - yuvRange.x()) * yuvRange.y();
    auto cbcr = sample(chroma, uv);
    auto u = (cbcr.x() - yuvRange.z()) * yuvRange.w();
    auto v = (cbcr.y() - yuvRange.z()) * yuvRange.w();

    auto red = y + yuvMatrix.x() * v;
    auto green = y - yuvMatrix.y() * u - yuvMatrix.z() * v;
    auto blue = y + yuvMatrix.w() * u;

    // Coding ranges overshoot 0-1 slightly at the extremes, and a colour
    // outside it would blend wrong rather than simply clip.
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

// Built manually instead of through ShaderProgram::prepare(), which has no way
// to pick a blend mode; everything the renderer draws is alpha-blended.
GPU::RenderPipelineDescriptor
    blendedDescriptor(const GPU::ShaderLibrary& library,
                      const GPU::VertexLayout& vertexLayout,
                      int sampleCount)
{
    auto descriptor = GPU::RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = vertexLayout;
    descriptor.sampleCount = sampleCount;
    descriptor.blendMode = GPU::BlendMode::AlphaBlend;
    return descriptor;
}

// A 1x1 opaque-white texture so untextured fills reuse the textured path: the
// fill colour is the tint, multiplied by white. Its sampling is immaterial -
// one clamped texel reads the same through any filter - so the untextured
// entry points draw it through whichever program is already bound.
GPU::Texture makeWhiteTexture()
{
    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = 1;
    descriptor.height = 1;
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;

    return GPU::Device::shared().makeTexture(descriptor, whitePixel);
}
} // namespace

SpriteProgram::SpriteProgram(GPU::TextureSampling sampling,
                             Point logicalSize,
                             int sampleCount)
    : shader(sampling)
    , library(GPU::Device::shared(), shader.source())
    , pipeline(GPU::Device::shared(),
               blendedDescriptor(library, shader.vertexLayout(), sampleCount))
{
    shader.setVertices(unitQuad);
    shader.screenSize = Array {logicalSize.x, logicalSize.y};
}

Nv12Program::Nv12Program(GPU::TextureSampling sampling,
                         Point logicalSize,
                         int sampleCount)
    : shader(sampling)
    , library(GPU::Device::shared(), shader.source())
    , pipeline(GPU::Device::shared(),
               blendedDescriptor(library, shader.vertexLayout(), sampleCount))
{
    shader.setVertices(unitQuad);
    shader.screenSize = Array {logicalSize.x, logicalSize.y};
}

SpriteRenderer::SpriteRenderer(Point logicalSizeToUse, int sampleCountToUse)
    : logicalSize(logicalSizeToUse)
    , sampleCount(sampleCountToUse)
    , white(makeWhiteTexture())
{
}

SpriteProgram& SpriteRenderer::programFor(GPU::TextureSampling sampling)
{
    auto& slot = programs[GPU::samplingIndex(sampling)];

    if (!slot.has_value())
        slot.emplace(sampling, logicalSize, sampleCount);

    return *slot;
}

Nv12Program& SpriteRenderer::nv12ProgramFor(GPU::TextureSampling sampling)
{
    auto& slot = nv12Programs[GPU::samplingIndex(sampling)];

    if (!slot.has_value())
        slot.emplace(sampling, logicalSize, sampleCount);

    return *slot;
}

void SpriteRenderer::begin(GPU::RenderPass& passToUse)
{
    pass = &passToUse;

    // The pipeline is bound by the first draw instead of here, because which
    // one it should be depends on how that draw samples.
    boundProgram = -1;
}

void SpriteRenderer::drawQuad(const GPU::Texture& texture,
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
    const auto index = GPU::samplingIndex(sampling);
    auto& program = programFor(sampling);
    auto& shader = program.shader;

    if (index != boundProgram)
    {
        pass->setPipeline(program.pipeline);
        pass->setVertexBuffer(shader.vertices());
        boundProgram = index;
    }

    shader.origin = Array {origin.x, origin.y};
    shader.edgeX = Array {edgeX.x, edgeX.y};
    shader.edgeY = Array {edgeY.x, edgeY.y};
    shader.uv0 = Array {u0, v0};
    shader.uv1 = Array {u1, v1};
    shader.tint = Array {tint.r, tint.g, tint.b, tint.a};
    shader.image = texture;

    pass->setVertexUniforms(shader);
    pass->setFragmentUniforms(shader);
    shader.bindTextures(*pass);
    pass->draw(6);
}

void SpriteRenderer::drawTexture(const GPU::Texture& texture,
                                 const Rect& dst,
                                 bool flipX,
                                 bool flipY,
                                 const Color& tint,
                                 GPU::TextureSampling sampling)
{
    drawQuad(texture,
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

    drawQuad(texture,
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
    drawQuad(texture, origin, edgeX, edgeY, 0.0f, 0.0f, 1.0f, 1.0f, tint, sampling);
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
    // Above the sprite programs in the same numbering, so switching between a
    // video quad and an overlay rebinds exactly once either way.
    const auto index = GPU::samplingConfigurations + GPU::samplingIndex(sampling);
    auto& program = nv12ProgramFor(sampling);
    auto& shader = program.shader;

    if (index != boundProgram)
    {
        pass->setPipeline(program.pipeline);
        pass->setVertexBuffer(shader.vertices());
        boundProgram = index;
    }

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

    pass->setVertexUniforms(shader);
    pass->setFragmentUniforms(shader);
    shader.bindTextures(*pass);
    pass->draw(6);
}

void SpriteRenderer::fillRect(const Rect& rect, const Color& color)
{
    drawQuad(white,
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

    // Top and bottom span the full width; the sides fit between them so corners
    // are not drawn twice (which would double-blend a translucent outline).
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

    // The segment normal, scaled to the half thickness: offsets each endpoint to
    // the two long edges of the quad.
    const auto nx = -delta.y / length * half;
    const auto ny = delta.x / length * half;

    drawQuad(white,
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
