#pragma once

#include <CoreGraphics/CoreGraphics.h>

#include <eacp/Core/ObjC/CFRef.h>
#include <eacp/Core/ObjC/ObjC.h>

#include <string>

#include "Primitives.h"

namespace eacp::Graphics
{
class Image;

CGRect toCGRect(const Rect& r);
Rect toRect(const CGRect& r);
CGPoint toCGPoint(const Point& p);
CFRef<CGColorRef> toCGColor(const Color& c);
CFRef<CGGradientRef> toCGGradient(const LinearGradient& gradient);

namespace detail
{
// Converts a CGImage into a straight-alpha 8-bit RGBA Image; sets error and
// returns an invalid Image on failure.
Image imageFromCGImage(CGImageRef image, std::string& error);
} // namespace detail

} // namespace eacp::Graphics