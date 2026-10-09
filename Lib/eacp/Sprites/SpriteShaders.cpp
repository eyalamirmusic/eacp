#include "SpriteShaders.h"
#include "SpriteRenderer.h"

namespace eacp::Sprites
{
SpriteShader::SpriteShader(GPU::TextureSampling sampling)
{
    image.sampling = sampling;
    compile();
}

// No EACP_SHADER_VALUE declaration for SpriteInstance: every field the shader
// pulls from it is a plain float[N], which the EDSL already maps to FloatN. The
// macro is only needed for structs with named components.

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
    auto fragmentUv = varying(uv);
    auto fragmentTint = varying(tint);
    setFragment(sample(image, fragmentUv) * fragmentTint);
}

Nv12Shader::Nv12Shader(GPU::TextureSampling sampling)
{
    luma.sampling = sampling;
    chroma.sampling = sampling;
    compile();
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
} // namespace eacp::Sprites
