#include "MacGraphicUtils.h"

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

Rect toRect(const CGRect& r)
{
    return {(float) r.origin.x,
            (float) r.origin.y,
            (float) r.size.width,
            (float) r.size.height};
}

CFRef<CGColorRef> toCGColor(const Color& c)
{
    static auto colorSpace = CFRef<CGColorSpaceRef>(CGColorSpaceCreateDeviceRGB());
    CGFloat components[4] = {c.r, c.g, c.b, c.a};
    return {CGColorCreate(colorSpace, components)};
}
}