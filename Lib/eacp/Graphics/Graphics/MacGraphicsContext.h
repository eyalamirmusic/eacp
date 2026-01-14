#pragma once

#include "../Primitives/MacGraphicUtils.h"
#include "GraphicsContext.h"
#include "../Primitives/Path.h"
#include "../Primitives/Font.h"

namespace eacp::Graphics
{
class MacOSContext final : public Context
{
public:

    explicit MacOSContext(CGContextRef contextToUse)
        : context(contextToUse)
        , currentColor{1.0f, 1.0f, 1.0f, 1.0f}
    {
        saveState();
    }

    ~MacOSContext() override { restoreState(); }

    void saveState() override { CGContextSaveGState(context); }
    void restoreState() override { CGContextRestoreGState(context); }

    void translate(float x, float y) override
    {
        CGContextTranslateCTM(context, x, y);
    }

    void scale(float x, float y) override { CGContextScaleCTM(context, x, y); }
    void rotate(float angle) override { CGContextRotateCTM(context, angle); }

    void fillRect(const Rect& r) override
    {
        CGContextFillRect(context, toCGRect(r));
    }

    void setColor(const Color& color) override
    {
        currentColor = color;
        CGContextSetRGBFillColor(context, color.r, color.g, color.b, color.a);
        CGContextSetRGBStrokeColor(context, color.r, color.g, color.b, color.a);
    }

    void fillRoundedRect(const Rect& r, float radius) override
    {
        auto p = Path();
        p.addRoundedRect(r, radius);
        fillPath(p);
    }

    void setCurrentPath(const Path& p)
    {
        CGContextAddPath(context, (CGPathRef) p.getHandle());
    }

    void fillPath(const Path& p) override
    {
        setCurrentPath(p);
        CGContextFillPath(context);
    }

    void setLineWidth(float width) override
    {
        CGContextSetLineWidth(context, width);
    }

    void strokeRect(const Rect& r) override
    {
        CGContextStrokeRect(context, toCGRect(r));
    }

    void strokePath(const Path& p) override
    {
        setCurrentPath(p);
        CGContextStrokePath(context);
    }

    void drawLine(const Point& start, const Point& end) override
    {
        auto p = Path();
        p.moveTo(start);
        p.lineTo(end);
        strokePath(p);
    }

    void drawText(const std::string& text, const Point& position, const Font& font) override;

private:
    CGContextRef context;
    Color currentColor;
};
} // namespace eacp::Graphics
