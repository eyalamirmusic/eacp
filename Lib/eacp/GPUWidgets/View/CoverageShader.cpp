#include "CoverageShader.h"

namespace eacp::GPUWidgets
{
CoverageShader::CoverageShader()
{
    mask.sampling = {GPU::TextureFilter::Nearest, GPU::TextureAddressMode::Clamp};
    compile();
}

void CoverageShader::define()
{
    auto corner = vertexInput(&FillVertex::position);

    auto pixelX = rect.x() + corner.x() * rect.z();
    auto pixelY = rect.y() + corner.y() * rect.w();

    setPosition(float4(pixelX / (viewport.x() * 0.5f) - 1.f,
                       1.f - pixelY / (viewport.y() * 0.5f),
                       0.f,
                       1.f));

    auto uv = varying(corner);
    setFragment(float4(color.xyz(), color.w() * sample(mask, uv).x()));
}

void CoverageShader::prepareQuad(int sampleCount)
{
    static const FillVertex quad[] = {
        {{0.f, 0.f}},
        {{1.f, 0.f}},
        {{0.f, 1.f}},
        {{1.f, 0.f}},
        {{1.f, 1.f}},
        {{0.f, 1.f}},
    };

    setVertices(quad);
    prepare(sampleCount,
            false,
            GPU::PrimitiveTopology::Triangles,
            GPU::BlendMode::AlphaBlend);
}

void CoverageShader::drawMask(GPU::RenderPass& pass,
                              const GPU::Texture& maskToDraw,
                              const Graphics::Rect& pixelRect,
                              const Graphics::Color& fill)
{
    rect = Array<float, 4> {pixelRect.x, pixelRect.y, pixelRect.w, pixelRect.h};
    color = fill;
    mask = maskToDraw;

    pass.draw(*this);
}

void CoverageShader::setViewport(float pixelWidth, float pixelHeight)
{
    viewport = Array<float, 2> {pixelWidth, pixelHeight};
}
} // namespace eacp::GPUWidgets
