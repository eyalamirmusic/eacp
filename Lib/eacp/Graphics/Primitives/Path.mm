#include "Path.h"
#include "GraphicUtils.h"

namespace eacp::Graphics
{

struct Path::Native
{
    Native() { clear(); }
    ~Native() { release(); }

    void release() const
    {
        if (handle)
            CGPathRelease(handle);
    }

    void clear()
    {
        release();
        handle = CGPathCreateMutable();
    }

    CGMutablePathRef handle = nullptr;
};

Path::Path()
    : impl()
{
}

void Path::moveTo(const Point& target)
{
    CGPathMoveToPoint(impl->handle, nullptr, target.x, target.y);
}

void Path::lineTo(const Point& target)
{
    CGPathAddLineToPoint(impl->handle, nullptr, target.x, target.y);
}

void Path::clear()
{
    impl->clear();
}

void Path::quadTo(float cx, float cy, float x, float y)
{
    CGPathAddQuadCurveToPoint(impl->handle, nullptr, cx, cy, x, y);
}

void Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    CGPathAddCurveToPoint(impl->handle, nullptr, c1x, c1y, c2x, c2y, x, y);
}

void Path::close()
{
    CGPathCloseSubpath(impl->handle);
}

void Path::addRect(const Rect& r)
{
    CGPathAddRect(impl->handle, nullptr, toCGRect(r));
}

void Path::addRoundedRect(const Rect& r, float radius)
{
    CGPathAddRoundedRect(impl->handle, nullptr, toCGRect(r), radius, radius);
}

void Path::addEllipse(const Rect& r)
{
    CGPathAddEllipseInRect(impl->handle, nullptr, toCGRect(r));
}

Path Path::scaled(float sx, float sy) const
{
    auto transform = CGAffineTransformMakeScale(sx, sy);
    auto transformed =
        CGPathCreateCopyByTransformingPath(impl->handle, &transform);

    Path result;
    CGPathRelease(result.impl->handle);
    result.impl->handle = CGPathCreateMutableCopy(transformed);
    CGPathRelease(transformed);

    return result;
}

void* Path::getHandle() const
{
    return impl->handle;
}

} // namespace eacp::Graphics