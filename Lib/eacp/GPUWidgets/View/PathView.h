#pragma once

#include "../Path/Path.h"

#include <eacp/Core/Utils/Pimpl.h>
#include <eacp/GPU/View/GPUView.h>
#include <eacp/Graphics/Primitives/Primitives.h>

namespace eacp::GPU
{
class Frame;
}

namespace eacp::GPUWidgets
{
// A GPU-rendered view that fills and/or strokes a Path. It tessellates the path on
// the CPU (ear clipping for fills, an offset ribbon for strokes) and draws the
// triangles through EDSL-authored shaders, exercising the whole GPU module: shader
// codegen, vertex buffers, uniform blocks and per-vertex colour.
//
// A fill is either a solid colour or a linear gradient; gradients are baked into a
// per-vertex-colour mesh. A stroke is a solid colour of a given width, drawn on top
// of the fill. Path coordinates map to the view's local point bounds by default;
// call setCoordinateSpace to author in a fixed logical space stretched to fill the
// view (an SVG-style view box) so shapes fill the view at any size.
class PathView : public GPU::GPUView
{
public:
    PathView();
    ~PathView() override;

    // The path to fill / stroke. Re-tessellated on the next render.
    void setPath(const Path& newPath);

    // Fill the path with a solid colour (the default fill is opaque white).
    void setFillColor(const Graphics::Color& color);

    // Fill the path with a linear gradient, sampled per fill vertex. The gradient's
    // start / end points are in the same coordinate space as the path.
    void setFillGradient(const Graphics::LinearGradient& gradient);

    // Whether the interior is filled at all. Turn off to draw an outline only.
    void setFilled(bool filled);

    // Stroke the path outline with a solid colour of the given width, drawn over
    // the fill. A width of 0 (the default) draws no stroke. The width is in path
    // coordinate units.
    void setStrokeColor(const Graphics::Color& color);
    void setStrokeWidth(float width);

    // Colour the view is cleared to before the fill and stroke are drawn.
    void setBackgroundColor(const Graphics::Color& color);

    // The logical coordinate space path points are expressed in, stretched to fill
    // the view. Pass {0, 0} (the default) to map 1:1 to the view's point bounds.
    void setCoordinateSpace(float width, float height);

    void render(GPU::Frame& frame) override;

private:
    struct Impl;
    Pimpl<Impl> impl;
};
} // namespace eacp::GPUWidgets
