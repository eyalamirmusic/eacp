#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include "Primitives.h"

namespace eacp::Graphics
{
CGRect toCGRect(const Rect& r);
Rect toRect(const CGRect& r);
CGPoint toCGPoint(const Point& p);
} // namespace eacp::Graphics