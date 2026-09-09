#include "SizeConstraint.h"

#include <algorithm>
#include <cmath>

namespace eacp::Graphics
{

ResizeAxis resizeAxisBetween(Point dragStart, Point proposed)
{
    auto widthMoved = proposed.x != dragStart.x;
    auto heightMoved = proposed.y != dragStart.y;

    if (widthMoved && !heightMoved)
        return ResizeAxis::Width;

    if (heightMoved && !widthMoved)
        return ResizeAxis::Height;

    return ResizeAxis::Both;
}

Point fitWithin(const SizeConstraint& constraint, Point available)
{
    auto fits = [available](Point size)
    { return size.x <= available.x && size.y <= available.y; };

    auto byWidth = constraint({available, ResizeAxis::Width});

    if (fits(byWidth))
        return byWidth;

    auto byHeight = constraint({available, ResizeAxis::Height});

    if (fits(byHeight))
        return byHeight;

    return {std::min(byWidth.x, available.x), std::min(byWidth.y, available.y)};
}

AspectRatioLock::AspectRatioLock(Point ratioToUse, Insets fixedToUse)
    : ratio(ratioToUse)
    , fixed(fixedToUse)
{
}

bool AspectRatioLock::isLocking() const
{
    return ratio.x > 0.f && ratio.y > 0.f;
}

Point AspectRatioLock::operator()(const ResizeRequest& request) const
{
    if (!isLocking())
        return request.size;

    auto border = fixed.size();
    auto locked = request.size - border;
    locked.x = std::max(locked.x, 1.f);
    locked.y = std::max(locked.y, 1.f);

    auto proportion = ratio.x / ratio.y;

    if (request.axis == ResizeAxis::Height)
        locked.x = std::round(locked.y * proportion);
    else
        locked.y = std::round(locked.x / proportion);

    return locked + border;
}

Rect AspectRatioLock::lockedArea(const Rect& bounds) const
{
    auto area = bounds.inset(fixed);

    if (area.isEmpty())
        return area.withSize(std::max(area.w, 0.f), std::max(area.h, 0.f));

    if (!isLocking())
        return area;

    auto proportion = ratio.x / ratio.y;
    auto width = area.w;
    auto height = width / proportion;

    if (height > area.h)
    {
        height = area.h;
        width = height * proportion;
    }

    return {area.x + (area.w - width) / 2.f,
            area.y + (area.h - height) / 2.f,
            width,
            height};
}

} // namespace eacp::Graphics
