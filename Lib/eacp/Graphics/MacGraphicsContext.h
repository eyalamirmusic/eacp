#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include "GraphicsContext.h"

namespace eacp::Graphics
{
CGRect toCGRect(const Rect& r)
{
    return CGRectMake(r.x, r.y, r.w, r.h);
}

CGPoint toCGPoint(const Point& p)
{
    return CGPointMake(p.x, p.y);
}

class MacOSContext final : public Context
{
public:
    MacOSContext(CGContextRef ctx)
        : context(ctx)
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
        updateFillColor();
        CGContextFillRect(context, toCGRect(r));
    }

    void updateFillColor()
    {
        CGContextSetRGBFillColor(
            context, lastColor.r, lastColor.g, lastColor.b, lastColor.a);
    }

    void updateStrokeColor()
    {
        CGContextSetRGBStrokeColor(
            context, lastColor.r, lastColor.g, lastColor.b, lastColor.a);
    }

    void fillRoundedRect(const Rect& r, float radius) override
    {
        updateFillColor();
        CGContextBeginPath(context);
        CGContextAddPath(
            context,
            CGPathCreateWithRoundedRect(toCGRect(r), radius, radius, nullptr));
        CGContextFillPath(context);
    }

    void setLineWidth(float width) override
    {
        CGContextSetLineWidth(context, width);
    }

    void strokeRect(const Rect& r) override
    {
        updateStrokeColor();
        CGContextStrokeRect(context, toCGRect(r));
    }

    void drawLine(const Point& start, const Point& end) override
    {
        updateStrokeColor();
        CGContextBeginPath(context);
        CGContextMoveToPoint(context, start.x, start.y);
        CGContextAddLineToPoint(context, end.x, end.y);
        CGContextStrokePath(context);
    }

private:
    CGContextRef context;
};
} // namespace eacp::Graphics
