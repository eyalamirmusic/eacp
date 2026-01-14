#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include "../ObjC/CFRef.h"
#include "../ObjC/ObjC.h"
#include "Primitives.h"

namespace eacp::Graphics
{
CGRect toCGRect(const Rect& r);
Rect toRect(const CGRect& r);
CGPoint toCGPoint(const Point& p);
CFRef<CGColorRef> toCGColor(const Color& c);

} // namespace eacp::Graphics