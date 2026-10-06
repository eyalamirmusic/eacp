#include "Mat4.h"

#include <cmath>

namespace eacp::Maths
{
Mat4 Mat4::rotationX(float radians)
{
    auto c = std::cos(radians);
    auto s = std::sin(radians);

    auto result = Mat4 {};
    result.setColumn(1, {0.f, c, s, 0.f});
    result.setColumn(2, {0.f, -s, c, 0.f});

    return result;
}

Mat4 Mat4::rotationY(float radians)
{
    auto c = std::cos(radians);
    auto s = std::sin(radians);

    auto result = Mat4 {};
    result.setColumn(0, {c, 0.f, -s, 0.f});
    result.setColumn(2, {s, 0.f, c, 0.f});

    return result;
}

Mat4 Mat4::rotationZ(float radians)
{
    auto c = std::cos(radians);
    auto s = std::sin(radians);

    auto result = Mat4 {};
    result.setColumn(0, {c, s, 0.f, 0.f});
    result.setColumn(1, {-s, c, 0.f, 0.f});

    return result;
}

Mat4 Mat4::perspective(float aspect, float fovY, float nearZ, float farZ)
{
    auto focal = 1.f / std::tan(fovY * 0.5f);

    auto result = Mat4 {};
    result.setColumn(0, {focal / aspect, 0.f, 0.f, 0.f});
    result.setColumn(1, {0.f, focal, 0.f, 0.f});
    result.setColumn(2, {0.f, 0.f, farZ / (nearZ - farZ), -1.f});
    result.setColumn(3, {0.f, 0.f, (farZ * nearZ) / (nearZ - farZ), 0.f});

    return result;
}

Mat4 Mat4::lookAt(const Vec3& eye, const Vec3& target, const Vec3& up)
{
    // Backward rather than forward: the camera looks down -z, so the third
    // basis vector of a right-handed frame points behind it.
    auto back = normalize(eye - target);
    auto right = normalize(cross(up, back));
    auto trueUp = cross(back, right);

    auto result = Mat4 {};
    result.setColumn(0, {right.x, trueUp.x, back.x, 0.f});
    result.setColumn(1, {right.y, trueUp.y, back.y, 0.f});
    result.setColumn(2, {right.z, trueUp.z, back.z, 0.f});
    result.setColumn(3, {-dot(right, eye), -dot(trueUp, eye), -dot(back, eye), 1.f});

    return result;
}
} // namespace eacp::Maths
