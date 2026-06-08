#include "PathView.h"

#include "../Gradient.h"
#include "../Path/PathTessellator.h"
#include "PathFillShader.h"
#include "VertexColorShader.h"

#include <eacp/GPU/Frame/Frame.h>
#include <eacp/GPU/Frame/RenderPass.h>

#include <algorithm>
#include <array>

namespace eacp::GPUWidgets
{
struct PathView::Impl
{
    enum class FillMode
    {
        Solid,
        Gradient
    };

    // Distinct shader instances so fill and stroke each own their own vertex
    // buffer: re-uploading one shader's buffer must not disturb a draw already
    // recorded from another in the same pass.
    PathFillShader fillSolidShader; // solid fill
    VertexColorShader gradientShader; // gradient fill (per-vertex colour)
    PathFillShader strokeShader; // solid stroke

    Path path;

    bool filled = true;
    FillMode fillMode = FillMode::Solid;
    Graphics::Color fillColor = Graphics::Color::white();
    Graphics::LinearGradient fillGradient;

    Graphics::Color strokeColor = Graphics::Color::black();
    float strokeWidth = 0.0f;

    Graphics::Color backgroundColor {0.08f, 0.09f, 0.12f, 1.0f};
    float spaceWidth = 0.0f;
    float spaceHeight = 0.0f;

    Vector<FillVertex> fillSolidVertices;
    Vector<GradientVertex> fillGradientVertices;
    Vector<FillVertex> strokeVertices;

    bool dirty = true;
    bool fillSolidReady = false;
    bool gradientReady = false;
    bool strokeReady = false;
};

PathView::PathView() = default;
PathView::~PathView() = default;

void PathView::setPath(const Path& newPath)
{
    impl->path = newPath;
    impl->dirty = true;
    repaint();
}

void PathView::setFillColor(const Graphics::Color& color)
{
    impl->fillMode = Impl::FillMode::Solid;
    impl->fillColor = color;
    impl->filled = true;
    impl->dirty = true;
    repaint();
}

void PathView::setFillGradient(const Graphics::LinearGradient& gradient)
{
    impl->fillMode = Impl::FillMode::Gradient;
    impl->fillGradient = gradient;
    impl->filled = true;
    impl->dirty = true;
    repaint();
}

void PathView::setFilled(bool filled)
{
    impl->filled = filled;
    impl->dirty = true;
    repaint();
}

void PathView::setStrokeColor(const Graphics::Color& color)
{
    impl->strokeColor = color;
    repaint();
}

void PathView::setStrokeWidth(float width)
{
    impl->strokeWidth = width;
    impl->dirty = true;
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

    auto prepareOnce = [this](GPU::ShaderProgram& shader, bool& ready)
    {
        if (!ready)
        {
            shader.prepare(sampleCount());
            ready = true;
        }
    };

    if (state.dirty)
    {
        state.fillSolidVertices.clear();
        state.fillGradientVertices.clear();
        state.strokeVertices.clear();

        if (state.filled)
        {
            auto positions = tessellateFill(state.path);

            if (state.fillMode == Impl::FillMode::Gradient)
            {
                state.fillGradientVertices.reserve(positions.size());

                for (const auto& point: positions)
                    state.fillGradientVertices.add(
                        {point, colorAt(state.fillGradient, point)});

                if (!state.fillGradientVertices.empty())
                    state.gradientShader.setVertices(
                        state.fillGradientVertices.data(),
                        state.fillGradientVertices.size());
            }
            else
            {
                state.fillSolidVertices.reserve(positions.size());

                for (const auto& point: positions)
                    state.fillSolidVertices.add(FillVertex {point});

                if (!state.fillSolidVertices.empty())
                    state.fillSolidShader.setVertices(
                        state.fillSolidVertices.data(),
                        state.fillSolidVertices.size());
            }
        }

        if (state.strokeWidth > 0.0f)
        {
            auto strokeTriangles = tessellateStroke(state.path, state.strokeWidth);
            state.strokeVertices.reserve(strokeTriangles.size());

            for (const auto& point: strokeTriangles)
                state.strokeVertices.add(FillVertex {point});

            if (!state.strokeVertices.empty())
                state.strokeShader.setVertices(state.strokeVertices.data(),
                                               state.strokeVertices.size());
        }

        state.dirty = false;
    }

    auto bounds = getLocalBounds();
    auto width = state.spaceWidth > 0.0f ? state.spaceWidth : bounds.w;
    auto height = state.spaceHeight > 0.0f ? state.spaceHeight : bounds.h;
    auto viewport = std::array {std::max(width, 1.0f), std::max(height, 1.0f)};

    auto solidColor = [](const Graphics::Color& c)
    { return std::array {c.r, c.g, c.b, c.a}; };

    auto pass = frame.beginPass({state.backgroundColor});

    if (state.filled)
    {
        if (state.fillMode == Impl::FillMode::Gradient
            && !state.fillGradientVertices.empty())
        {
            prepareOnce(state.gradientShader, state.gradientReady);
            state.gradientShader.viewport = viewport;
            pass.draw(state.gradientShader);
        }
        else if (state.fillMode == Impl::FillMode::Solid
                 && !state.fillSolidVertices.empty())
        {
            prepareOnce(state.fillSolidShader, state.fillSolidReady);
            state.fillSolidShader.viewport = viewport;
            state.fillSolidShader.color = solidColor(state.fillColor);
            pass.draw(state.fillSolidShader);
        }
    }

    if (state.strokeWidth > 0.0f && !state.strokeVertices.empty())
    {
        prepareOnce(state.strokeShader, state.strokeReady);
        state.strokeShader.viewport = viewport;
        state.strokeShader.color = solidColor(state.strokeColor);
        pass.draw(state.strokeShader);
    }
}
} // namespace eacp::GPUWidgets
