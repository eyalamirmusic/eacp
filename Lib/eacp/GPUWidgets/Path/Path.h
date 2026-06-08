#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Graphics/Primitives/Primitives.h>

namespace eacp::GPUWidgets
{
// A 2D vector path built from lines, Bezier curves and primitive shapes, ready to
// fill on the GPU. The geometry-owning sibling of eacp::Graphics::Path: where that
// class wraps an opaque native CGPath / Direct2D handle, this one stores its
// sub-paths as flattened polylines, so a tessellator can read the actual points.
// Curves and arcs are flattened to line segments as the path is built.
//
//   Path path;
//   path.addRoundedRect ({10, 10, 200, 120}, 16.0f);
//   path.moveTo ({20, 20});
//   path.cubicTo (40, 0, 80, 120, 120, 40);
//   path.close();
//
// Method names mirror eacp::Graphics::Path so the two read as siblings.
class Path
{
public:
    // One flattened sub-path: a polyline, optionally marked closed. A fill always
    // treats a sub-path as closed; the flag is kept for correctness and future
    // stroking work.
    struct SubPath
    {
        Vector<Graphics::Point> points;
        bool closed = false;
    };

    Path() = default;

    void clear();
    bool isEmpty() const;

    // Starts a new sub-path at target. Subsequent line/curve calls extend it.
    void moveTo(const Graphics::Point& target);
    void lineTo(const Graphics::Point& target);

    // Quadratic / cubic Bezier from the current point, flattened to line segments.
    void quadTo(float controlX, float controlY, float endX, float endY);
    void cubicTo(float control1X,
                 float control1Y,
                 float control2X,
                 float control2Y,
                 float endX,
                 float endY);

    void close();

    // Primitive shapes, each added as its own closed sub-path.
    void addRect(const Graphics::Rect& rect);
    void addRoundedRect(const Graphics::Rect& rect, float cornerRadius);
    void addEllipse(const Graphics::Rect& rect);

    // The smallest rectangle containing every point; an empty rect for an empty
    // path.
    Graphics::Rect getBounds() const;

    const Vector<SubPath>& getSubPaths() const { return subPaths; }

private:
    SubPath& currentSubPath();

    Vector<SubPath> subPaths;
    Graphics::Point currentPoint;
};
} // namespace eacp::GPUWidgets
