#pragma once

#include "AffineTransform.h"

namespace eacp::GPUWidgets
{
// The geometry-owning sibling of eacp::Graphics::Path: stores readable flattened
// polylines rather than an opaque CGPath / Direct2D handle.
class Path
{
public:
    // A fill treats every sub-path as closed regardless of `closed`, which only
    // stroking honours.
    struct SubPath
    {
        Vector<Graphics::Point> points;
        bool closed = false;
    };

    Path() = default;

    void clear();
    bool isEmpty() const;

    // Maximum deviation of a flattened polyline from its curve, in path units.
    // Only affects curves added after the call.
    void setFlatness(float toleranceInPathUnits);
    float getFlatness() const { return flatness; }

    // Starts a new sub-path; line and curve calls extend the current one.
    void moveTo(const Graphics::Point& target);
    void lineTo(const Graphics::Point& target);

    // Flattened to line segments as they are added.
    void quadTo(float controlX, float controlY, float endX, float endY);
    void cubicTo(float control1X,
                 float control1Y,
                 float control2X,
                 float control2Y,
                 float endX,
                 float endY);

    void close();

    // Each added as its own closed sub-path.
    void addRect(const Graphics::Rect& rect);
    void addRoundedRect(const Graphics::Rect& rect, float cornerRadius);
    void addEllipse(const Graphics::Rect& rect);

    // Sub-paths are merged, not combined: under the non-zero rule that unions
    // same-wound regions and subtracts oppositely wound ones.
    void append(const Path& other);

    // Transforms the already-flattened points, so scaling up magnifies the
    // flattening error; set flatness for the final scale before adding curves.
    Path transformed(const AffineTransform& transform) const;
    Path scaled(float scaleX, float scaleY) const;

    // An empty rect for an empty path.
    Graphics::Rect getBounds() const;

    const Vector<SubPath>& getSubPaths() const { return subPaths; }

private:
    SubPath& currentSubPath();

    int segmentsForCurve(float secondDifference) const;
    int segmentsForArc(float radius, float sweep) const;

    Vector<SubPath> subPaths;
    Graphics::Point currentPoint;
    float flatness = 0.05f;
};
} // namespace eacp::GPUWidgets
