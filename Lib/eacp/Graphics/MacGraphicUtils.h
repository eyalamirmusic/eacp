#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include "Primitives.h"

namespace eacp::Graphics
{
CGRect toCGRect(const Rect& r);
CGPoint toCGPoint(const Point& p);
}