#pragma once

#include "Primitives.h"

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

    void setColor(const Color& color) { lastColor = color; }

    virtual void fillRect(const Rect& rect) = 0;
    virtual void fillRoundedRect(const Rect& rect,
                                 float radius) = 0; // Very easy in CG

    virtual void setLineWidth(float width) = 0;
    virtual void strokeRect(const Rect& rect) = 0;
    virtual void drawLine(const Point& start, const Point& end) = 0;

    Color lastColor {};
};
} // namespace eacp::Graphics