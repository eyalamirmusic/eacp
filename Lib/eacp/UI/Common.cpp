#include "Common.h"

#include <algorithm>

namespace eacp::UI
{
namespace
{
float radiusScaleForSide(float side, float first, float second)
{
    auto wanted = first + second;

    return wanted > side ? side / wanted : 1.f;
}
} // namespace

CornerRadii fittedRadii(const RoundedRect& shape)
{
    if (shape.isEmpty())
        return {};

    auto radii = CornerRadii {std::max(0.f, shape.radii.topLeft),
                              std::max(0.f, shape.radii.topRight),
                              std::max(0.f, shape.radii.bottomRight),
                              std::max(0.f, shape.radii.bottomLeft)};

    auto width = shape.rect.w;
    auto height = shape.rect.h;

    auto factor = std::min(
        {radiusScaleForSide(width, radii.topLeft, radii.topRight),
         radiusScaleForSide(width, radii.bottomLeft, radii.bottomRight),
         radiusScaleForSide(height, radii.topLeft, radii.bottomLeft),
         radiusScaleForSide(height, radii.topRight, radii.bottomRight)});

    return {radii.topLeft * factor,
            radii.topRight * factor,
            radii.bottomRight * factor,
            radii.bottomLeft * factor};
}

bool contains(const Rect& outer, const Rect& inner)
{
    if (inner.isEmpty())
        return true;

    return inner.left() >= outer.left() && inner.right() <= outer.right()
           && inner.top() >= outer.top() && inner.bottom() <= outer.bottom();
}
} // namespace eacp::UI
