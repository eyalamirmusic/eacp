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
}