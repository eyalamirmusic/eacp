#include "Primitives.h"

namespace eacp::Graphics
{
Point::Point(float xToUse, float yToUse)
    : x(xToUse)
    , y(yToUse)
{
}

Rect::Rect(float xToUse, float yToUse, float wToUse, float hToUse)
    : x(xToUse)
    , y(yToUse)
    , w(wToUse)
    , h(hToUse)
{
}

Color::Color(float rToUse, float gToUse, float bToUse, float aToUse)
    : r(rToUse)
    , g(gToUse)
    , b(bToUse)
    , a(aToUse)
{
}
}