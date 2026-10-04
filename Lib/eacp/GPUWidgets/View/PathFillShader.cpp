#include "PathFillShader.h"

namespace eacp::GPUWidgets
{
PathFillShader::PathFillShader()
{
    compile();
}

void PathFillShader::define()
{
    auto position = vertexInput(&FillVertex::position);

    auto clipX = position.x() / (viewport.x() * 0.5f) - 1.0f;
    auto clipY = 1.0f - position.y() / (viewport.y() * 0.5f);

    setPosition(float4(clipX, clipY, 0.0f, 1.0f));
    setFragment(color);
}
} // namespace eacp::GPUWidgets
