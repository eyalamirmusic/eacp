#pragma once

#include <CoreGraphics/CoreGraphics.h>

#include <eacp/Core/ObjC/CFRef.h>
#include <eacp/Core/ObjC/ObjC.h>

#include "Primitives.h"

namespace eacp::Graphics
{
CGRect toCGRect(const Rect& r);
Rect toRect(const CGRect& r);
CGPoint toCGPoint(const Point& p);
CFRef<CGColorRef> toCGColor(const Color& c);
CFRef<CGGradientRef> toCGGradient(const LinearGradient& gradient);

} // namespace eacp::Graphics