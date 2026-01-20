#include "GraphicUtils.h"

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

    std::vector<CGFloat> components;
    std::vector<CGFloat> locations;

    for (const auto& stop : gradient.stops)
    {
        components.push_back(stop.color.r);
        components.push_back(stop.color.g);
        components.push_back(stop.color.b);
        components.push_back(stop.color.a);
        locations.push_back(stop.position);
    }

    return {CGGradientCreateWithColorComponents(colorSpace,
                                                components.data(),
                                                locations.data(),
                                                gradient.stops.size())};
}
}