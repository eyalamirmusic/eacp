#include "PathView.h"

#include "../Path/PathTessellator.h"
#include "PathFillShader.h"

#include <eacp/GPU/Frame/Frame.h>
#include <eacp/GPU/Frame/RenderPass.h>

#include <algorithm>
#include <array>

namespace eacp::GPUWidgets
{
struct PathView::Impl
{
    PathFillShader shader;
    Vector<FillVertex> vertices;
    Path path;
    Graphics::Color fillColor = Graphics::Color::white();
    Graphics::Color backgroundColor {0.08f, 0.09f, 0.12f, 1.0f};
    float spaceWidth = 0.0f;
    float spaceHeight = 0.0f;
    bool pipelineReady = false;
    bool meshDirty = false;
};

PathView::PathView() = default;
PathView::~PathView() = default;

void PathView::setPath(const Path& newPath)
{
    impl->path = newPath;
    impl->meshDirty = true;
    repaint();
}

void PathView::setFillColor(const Graphics::Color& color)
{
    impl->fillColor = color;
    repaint();
}

void PathView::setBackgroundColor(const Graphics::Color& color)
{
    impl->backgroundColor = color;
    repaint();
}

void PathView::setCoordinateSpace(float width, float height)
{
    impl->spaceWidth = width;
    impl->spaceHeight = height;
    repaint();
}

void PathView::render(GPU::Frame& frame)
{
    auto& state = *impl;

    if (!state.pipelineReady)
    {
        state.shader.prepare(sampleCount());
        state.pipelineReady = true;
    }

    if (state.meshDirty)
    {
        auto triangles = tessellateFill(state.path);

        state.vertices.clear();
        state.vertices.reserve(triangles.size());

        for (const auto& point: triangles)
            state.vertices.add(FillVertex {point});

        if (!state.vertices.empty())
            state.shader.setVertices(state.vertices.data(), state.vertices.size());

        state.meshDirty = false;
    }

    auto bounds = getLocalBounds();
    auto width = state.spaceWidth > 0.0f ? state.spaceWidth : bounds.w;
    auto height = state.spaceHeight > 0.0f ? state.spaceHeight : bounds.h;

    state.shader.viewport =
        std::array {std::max(width, 1.0f), std::max(height, 1.0f)};
    state.shader.color = std::array {
        state.fillColor.r, state.fillColor.g, state.fillColor.b, state.fillColor.a};

    auto pass = frame.beginPass({state.backgroundColor});

    if (!state.vertices.empty())
        pass.draw(state.shader);
}
} // namespace eacp::GPUWidgets
