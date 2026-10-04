#include "AffineTransform.h"

#include <algorithm>
#include <cmath>

namespace eacp::GPUWidgets
{
AffineTransform AffineTransform::rotation(float radians)
{
    auto cosine = std::cos(radians);
    auto sine = std::sin(radians);

    return {cosine, sine, -sine, cosine, 0.f, 0.f};
}

AffineTransform AffineTransform::skew(float radiansX, float radiansY)
{
    return {1.f, std::tan(radiansY), std::tan(radiansX), 1.f, 0.f, 0.f};
}

AffineTransform AffineTransform::rotationAbout(float radians,
                                               const Graphics::Point& centre)
{
    return translation(-centre.x, -centre.y)
        .then(rotation(radians))
        .then(translation(centre.x, centre.y));
}

Graphics::Point AffineTransform::apply(const Graphics::Point& point) const
{
    return {a * point.x + c * point.y + tx, b * point.x + d * point.y + ty};
}

Graphics::Rect AffineTransform::apply(const Graphics::Rect& rect) const
{
    auto topLeft = apply({rect.x, rect.y});
    auto topRight = apply({rect.right(), rect.y});
    auto bottomLeft = apply({rect.x, rect.bottom()});
    auto bottomRight = apply({rect.right(), rect.bottom()});

    auto left = std::min(std::min(topLeft.x, topRight.x),
                         std::min(bottomLeft.x, bottomRight.x));
    auto right = std::max(std::max(topLeft.x, topRight.x),
                          std::max(bottomLeft.x, bottomRight.x));
    auto top = std::min(std::min(topLeft.y, topRight.y),
                        std::min(bottomLeft.y, bottomRight.y));
    auto bottom = std::max(std::max(topLeft.y, topRight.y),
                           std::max(bottomLeft.y, bottomRight.y));

    return {left, top, right - left, bottom - top};
}

float AffineTransform::getScaleFactor() const
{
    return std::sqrt(std::abs(a * d - b * c));
}

AffineTransform AffineTransform::inverted() const
{
    auto determinant = getDeterminant();

    if (std::abs(determinant) < 1e-12f)
        return {};

    auto inverseA = d / determinant;
    auto inverseB = -b / determinant;
    auto inverseC = -c / determinant;
    auto inverseD = a / determinant;

    return {inverseA,
            inverseB,
            inverseC,
            inverseD,
            -(inverseA * tx + inverseC * ty),
            -(inverseB * tx + inverseD * ty)};
}
} // namespace eacp::GPUWidgets
