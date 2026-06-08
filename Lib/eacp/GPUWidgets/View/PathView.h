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
// A GPU-rendered view that fills a Path with a solid colour. It tessellates the
// path on the CPU (ear clipping) and draws the triangles through the EDSL-authored
// PathFillShader. The whole GPU module is exercised end to end: shader codegen,
// vertex buffers, a vec2/vec4 uniform block and the render pass.
//
// Path coordinates map to the view's local point bounds by default. Call
// setCoordinateSpace to author in a fixed logical space stretched to fill the view
// instead (an SVG-style view box), which keeps shapes filling the view at any
// size.
class PathView : public GPU::GPUView
{
public:
    PathView();
    ~PathView() override;

    // The path to fill. Re-tessellated on the next render.
    void setPath(const Path& newPath);

    void setFillColor(const Graphics::Color& color);

    // Colour the view is cleared to before the fill is drawn.
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
