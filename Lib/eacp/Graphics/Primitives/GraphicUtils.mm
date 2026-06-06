#include "GraphicUtils.h"

#include <eacp/Core/Utils/Containers.h>

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

CFRef<CGGradientRef> toCGGradient(const LinearGradient& gradient)
{
    static auto colorSpace = CFRef<CGColorSpaceRef>(CGColorSpaceCreateDeviceRGB());

    auto components = Vector<CGFloat>();
    auto locations = Vector<CGFloat>();

    for (const auto& stop : gradient.stops)
    {
        components.add(stop.color.r);
        components.add(stop.color.g);
        components.add(stop.color.b);
        components.add(stop.color.a);
        locations.add(stop.position);
    }

    return {CGGradientCreateWithColorComponents(colorSpace,
                                                components.data(),
                                                locations.data(),
                                                (size_t) gradient.stops.size())};
}
}
