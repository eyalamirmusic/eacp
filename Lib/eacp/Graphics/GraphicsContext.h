#pragma once

#include "Path.h"

namespace eacp::Graphics
{
class Context
{
public:
    virtual ~Context() = default;

    virtual void saveState() = 0;
    virtual void restoreState() = 0;

    virtual void translate(float x, float y) = 0;
    virtual void scale(float x, float y) = 0;
    virtual void rotate(float angleRadians) = 0;

    virtual void setColor(const Color& color) = 0;

    virtual void fillRect(const Rect& rect) = 0;
    virtual void fillRoundedRect(const Rect& rect,
                                 float radius) = 0; // Very easy in CG

    virtual void setLineWidth(float width) = 0;
    virtual void strokeRect(const Rect& rect) = 0;
    virtual void drawLine(const Point& start, const Point& end) = 0;

    virtual void fillPath(const Path& p) = 0;
    virtual void strokePath(const Path& p) = 0;
};
} // namespace eacp::Graphics