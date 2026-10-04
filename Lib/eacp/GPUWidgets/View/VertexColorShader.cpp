#include "VertexColorShader.h"

namespace eacp::GPUWidgets
{
VertexColorShader::VertexColorShader()
{
    compile();
}

void VertexColorShader::define()
{
    auto position = vertexInput(&GradientVertex::position);
    auto color = vertexInput(&GradientVertex::color);
    auto fragColor = varying(color);

    auto clipX = position.x() / (viewport.x() * 0.5f) - 1.0f;
    auto clipY = 1.0f - position.y() / (viewport.y() * 0.5f);

    setPosition(float4(clipX, clipY, 0.0f, 1.0f));
    setFragment(fragColor);
}
} // namespace eacp::GPUWidgets
