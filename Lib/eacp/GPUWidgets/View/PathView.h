#pragma once

#include "../Path/Path.h"

namespace eacp::GPU
{
class Frame;
}

namespace eacp::GPUWidgets
{
// Fills and/or strokes a Path, tessellated on the CPU. The stroke is drawn over
// the fill, and a gradient fill is baked into a per-vertex-colour mesh.
class PathView : public GPU::GPUView
{
public:
    PathView();
    ~PathView() override;

    // Re-tessellated on the next render.
    void setPath(const Path& newPath);

    void setFillColor(const Graphics::Color& color);

    // Sampled per fill vertex; its start / end are in the path's coordinates.
    void setFillGradient(const Graphics::LinearGradient& gradient);

    void setFilled(bool filled);

    // Width is in path coordinate units; 0, the default, draws no stroke.
    void setStrokeColor(const Graphics::Color& color);
    void setStrokeWidth(float width);

    // Cleared to before the fill and stroke are drawn.
    void setBackgroundColor(const Graphics::Color& color);

    // The logical space path points are in, stretched to fill the view. {0, 0},
    // the default, maps 1:1 to the view's point bounds.
    void setCoordinateSpace(float width, float height);

    void render(GPU::Frame& frame) override;

private:
    struct Impl;
    Pimpl<Impl> impl;
};
} // namespace eacp::GPUWidgets
