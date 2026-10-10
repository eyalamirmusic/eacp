#pragma once

#include "AffineTransform.h"

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

    // How far a flattened polyline may stray from the curve it replaces, in path
    // units. Curves are subdivided until they hold to it, so a big shape gets
    // more segments than a small one instead of every shape getting the same
    // fixed count - which is what made a large circle read as the polygon it was.
    //
    // The default keeps the error under a tenth of a device pixel at the usual
    // 2x backing scale, which is below anything an 8-bit coverage channel can
    // express. Segment count grows as 1/sqrt of this, so loosening it to save
    // work buys little and costs visibly; tighten it only for a shape rasterized
    // far above its authored size. Set before the curves are added.
    void setFlatness(float toleranceInPathUnits);
    constexpr float getFlatness() const { return flatness; }

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

    // Elliptical arcs, with juce::Path's semantics so a JUCE port maps one to
    // one. Angles are in radians measured clockwise from the 12 o'clock
    // position: 0 is the top of the ellipse, pi / 2 its right-hand side. The
    // arc runs from fromRadians to toRadians in whichever direction that is,
    // so a reversed pair sweeps anticlockwise and a span of 2 pi or more goes
    // the whole way round. With startAsNewSubPath false the arc joins the
    // current sub-path with a line to its first point; true starts a new one.
    //
    // Each quarter turn or less of the sweep is one cubic Bezier, so the arc
    // is flattened to the same tolerance as any other curve.
    void addArc(const Graphics::Rect& ellipseBounds,
                float fromRadians,
                float toRadians,
                bool startAsNewSubPath = false);

    // The same, about a centre, with the ellipse turned clockwise by
    // rotationOfEllipse radians about it before the arc is traced.
    void addCentredArc(float centreX,
                       float centreY,
                       float radiusX,
                       float radiusY,
                       float rotationOfEllipse,
                       float fromRadians,
                       float toRadians,
                       bool startAsNewSubPath = false);

    // A closed wedge of the ellipse between the two angles, as a new sub-path.
    // innerCircleProportionalSize 0 runs it to the centre; between 0 and 1 it
    // is a ring segment whose inner edge is the ellipse scaled by that much.
    // A full turn with a hole is two sub-paths, the inner one wound the other
    // way so the non-zero rule leaves it empty.
    void addPieSegment(const Graphics::Rect& ellipseBounds,
                       float fromRadians,
                       float toRadians,
                       float innerCircleProportionalSize);

    // The same geometry with every point mapped through `transform`.
    //
    // It maps the polyline, not the curves it was flattened from, so the segment
    // count is whatever the flatness in force when the curves were added asked
    // for. Scaling a path up afterwards therefore magnifies the flattening error
    // with it, and a circle built at its authored size and then scaled by ten
    // reads as the polygon it always was. Set setFlatness to the tolerance the
    // *result* needs - the authored tolerance divided by
    // transform.getScaleFactor() - before building anything meant to be
    // transformed.
    // Another path's sub-paths added to this one's, which under the non-zero
    // rule is the union of the two regions -- what a clip path made of several
    // shapes is, and what a stroke and the shape it outlines are not, those
    // being wound against each other on purpose.
    void append(const Path& other);

    Path transformed(const AffineTransform& transform) const;

    // transformed, in place - juce::Path::applyTransform.
    void applyTransform(const AffineTransform& transform);
    Path scaled(float scaleX, float scaleY) const;

    // The smallest rectangle containing every point; an empty rect for an empty
    // path.
    Graphics::Rect getBounds() const;

    constexpr const Vector<SubPath>& getSubPaths() const { return subPaths; }

private:
    SubPath& currentSubPath();

    // Segments a curve or an arc needs to stay within flatness of the real
    // thing. Both fall out of the same fact: uniform subdivision cuts the
    // deviation by the square of the count.
    int segmentsForCurve(float secondDifference) const;
    int segmentsForArc(float radius, float sweep) const;

    Vector<SubPath> subPaths;
    Graphics::Point currentPoint;
    float flatness = 0.05f;
};
} // namespace eacp::GPUWidgets
