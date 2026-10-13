#include "Gradient.h"

namespace eacp::UI
{
Gradient Gradient::linear(Point from,
                          const Color& fromColour,
                          Point to,
                          const Color& toColour)
{
    auto gradient = Gradient {};
    gradient.start = from;
    gradient.end = to;
    gradient.stops = {{fromColour, 0.f}, {toColour, 1.f}};

    return gradient;
}

Gradient Gradient::radial(Point centre,
                          const Color& centreColour,
                          Point edge,
                          const Color& edgeColour)
{
    auto gradient = linear(centre, centreColour, edge, edgeColour);
    gradient.kind = Kind::Radial;
    gradient.radius = centre.distanceTo(edge);

    return gradient;
}

Gradient Gradient::vertical(const Color& top,
                            float topY,
                            const Color& bottom,
                            float bottomY)
{
    return linear({0.f, topY}, top, {0.f, bottomY}, bottom);
}

Gradient Gradient::vertical(const Color& top, const Color& bottom, const Rect& area)
{
    return vertical(top, area.top(), bottom, area.bottom());
}

Gradient Gradient::horizontal(const Color& left,
                              float leftX,
                              const Color& right,
                              float rightX)
{
    return linear({leftX, 0.f}, left, {rightX, 0.f}, right);
}

Gradient
    Gradient::horizontal(const Color& left, const Color& right, const Rect& area)
{
    return horizontal(left, area.left(), right, area.right());
}

Gradient& Gradient::addStop(float position, const Color& colour)
{
    stops.add({colour, position});
    return *this;
}

bool Gradient::isEmpty() const
{
    return stops.empty();
}
} // namespace eacp::UI
