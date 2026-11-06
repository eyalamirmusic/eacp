#pragma once

/** @file
    Vector paths drawn on the GPU, in portable C++ over eacp-gpu. Built where
    EACP_HAS_GPU is (Apple, Windows, Linux, Android).

    Path is the geometry: lines, curves, arcs and shapes, flattened to
    polylines as they are added (set the flatness first). Three ways to draw
    it: PathView, a GPUView that tessellates on the CPU and fills or strokes
    one path; tessellateFill, tessellateStroke or strokeToFill for a triangle
    mesh of your own; PathRasterizer, analytic antialiased coverage computed
    in compute kernels into a texture, which CoverageShader then paints.

    This Path is not Graphics::Path, which wraps an opaque native handle and
    needs EACP_HAS_CONTEXT. Inside a UI component tree, draw through
    UI::GPUComponent rather than adding a PathView as a native child view.
*/

#include "Gradient.h"
#include "Path/PathRasterizer.h"
#include "Path/PathStroker.h"
#include "Path/PathTessellator.h"
#include "View/CoverageShader.h"
#include "View/PathFillShader.h"
#include "View/PathView.h"
#include "View/VertexColorShader.h"

#include <eacp/GPU/GPU.h>
