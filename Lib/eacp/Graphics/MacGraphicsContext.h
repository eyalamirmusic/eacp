#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include "GraphicsContext.h"

namespace eacp::Graphics
{
CGRect toCGRect(const Rect& r)
{
    return CGRectMake(r.x, r.y, r.w, r.h);
}

class MacOSContext : public Context
{
public:
    MacOSContext(CGContextRef ctx)
        : context(ctx)
    {
    }

    void setFillColor(const Color& c) override
    {
        CGContextSetRGBFillColor(context, c.r, c.g, c.b, c.a);
    }

    void fillRect(const Rect& r) override
    {
        CGContextFillRect(context, toCGRect(r));
    }

private:
    CGContextRef context;
};
} // namespace eacp::Graphics
