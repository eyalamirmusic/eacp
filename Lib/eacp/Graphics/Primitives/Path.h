#pragma once

/** @file Vector paths for Context drawing and SVG geometry. */

#include "../Common.h"

#include "Primitives.h"

namespace eacp::Graphics
{

/// A vector outline in points. A Core Graphics path on Apple, a Direct2D geometry
/// on Windows, and on Linux and Android a recorded command list only (see
/// Path-Linux.h), since those have no 2D Context.
class Path
{
public:
    Path();

    void clear();

    void moveTo(const Point& target);
    void lineTo(const Point& target);

    void quadTo(float cx, float cy, float x, float y);
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);

    void close();

    void addRect(const Rect& rect);
    void addRoundedRect(const Rect& rect, float radius);
    void addEllipse(const Rect& rect);

    Path scaled(float sx, float sy) const;

    /// The native object: CGPathRef on Apple, ID2D1Geometry* on Windows,
    /// PathGeometry* on Linux and Android. Owned by the Path.
    void* getHandle() const;

private:
    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics