#include "SpriteRenderer.h"

#include <array>

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
// to enable blending; everything the renderer draws is alpha-blended.
GPU::RenderPipelineDescriptor
    blendedDescriptor(const GPU::ShaderLibrary& library,
                      const GPU::VertexLayout& vertexLayout,
                      int sampleCount)
{
    auto descriptor = GPU::RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = vertexLayout;
    descriptor.sampleCount = sampleCount;
    descriptor.blending = true;
    return descriptor;
}

// A 1x1 opaque-white texture so untextured fills reuse the textured path: the
// fill colour is the tint, multiplied by white.
GPU::Texture makeWhiteTexture()
{
    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = 1;
    descriptor.height = 1;
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;
    descriptor.filter = GPU::TextureFilter::Nearest;
    descriptor.addressMode = GPU::TextureAddressMode::Clamp;

    return GPU::Device::shared().makeTexture(descriptor, whitePixel);
}
} // namespace

SpriteRenderer::SpriteRenderer(Graphics::Point logicalSize, int sampleCount)
    : library(GPU::Device::shared(), shader.source())
    , pipeline(GPU::Device::shared(),
               blendedDescriptor(library, shader.vertexLayout(), sampleCount))
    , white(makeWhiteTexture())
{
    shader.setVertices(unitQuad);
    shader.screenSize = std::array {logicalSize.x, logicalSize.y};
}

void SpriteRenderer::begin(GPU::RenderPass& passToUse)
{
    pass = &passToUse;
    pass->setPipeline(pipeline);
    pass->setVertexBuffer(shader.vertices());
}

void SpriteRenderer::drawQuad(const GPU::Texture& texture,
                              Graphics::Point origin,
                              Graphics::Point edgeX,
                              Graphics::Point edgeY,
                              float u0,
                              float v0,
                              float u1,
                              float v1,
                              const Graphics::Color& tint)
{
    shader.origin = std::array {origin.x, origin.y};
    shader.edgeX = std::array {edgeX.x, edgeX.y};
    shader.edgeY = std::array {edgeY.x, edgeY.y};
    shader.uv0 = std::array {u0, v0};
    shader.uv1 = std::array {u1, v1};
    shader.tint = std::array {tint.r, tint.g, tint.b, tint.a};
    shader.image = texture;

    pass->setVertexUniforms(shader);
    pass->setFragmentUniforms(shader);
    shader.bindTextures(*pass);
    pass->draw(6);
}

void SpriteRenderer::drawTexture(const GPU::Texture& texture,
                                 const Graphics::Rect& dst,
                                 bool flipX,
                                 bool flipY,
                                 const Graphics::Color& tint)
{
    drawQuad(texture,
             {dst.x, dst.y},
             {dst.w, 0.0f},
             {0.0f, dst.h},
             flipX ? 1.0f : 0.0f,
             flipY ? 1.0f : 0.0f,
             flipX ? 0.0f : 1.0f,
             flipY ? 0.0f : 1.0f,
             tint);
}

void SpriteRenderer::drawTexture(const GPU::Texture& texture,
                                 const Graphics::Rect& src,
                                 const Graphics::Rect& dst,
                                 const Graphics::Color& tint)
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
             tint);
}

void SpriteRenderer::fillRect(const Graphics::Rect& rect,
                              const Graphics::Color& color)
{
    drawQuad(white,
             {rect.x, rect.y},
             {rect.w, 0.0f},
             {0.0f, rect.h},
             0.0f,
             0.0f,
             1.0f,
             1.0f,
             color);
}

void SpriteRenderer::drawRect(const Graphics::Rect& rect,
                              const Graphics::Color& color,
                              float thickness)
{
    const auto t = thickness;

    // Top and bottom span the full width; the sides fit between them so corners
    // are not drawn twice (which would double-blend a translucent outline).
    fillRect({rect.x, rect.y, rect.w, t}, color);
    fillRect({rect.x, rect.y + rect.h - t, rect.w, t}, color);
    fillRect({rect.x, rect.y + t, t, rect.h - 2.0f * t}, color);
    fillRect({rect.x + rect.w - t, rect.y + t, t, rect.h - 2.0f * t}, color);
}

void SpriteRenderer::drawLine(Graphics::Point a,
                              Graphics::Point b,
                              const Graphics::Color& color,
                              float thickness)
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
             color);
}
} // namespace eacp::Sprites
