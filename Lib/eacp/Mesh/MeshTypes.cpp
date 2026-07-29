#include "MeshTypes.h"

namespace eacp::Mesh
{
Mat4 Mat4::identity()
{
    auto result = Mat4 {};

    for (auto i = 0; i < 4; ++i)
        result.at(i, i) = 1.0f;

    return result;
}

Mat4 Mat4::translation(Vec3 offset)
{
    auto result = identity();
    result.at(3, 0) = offset.x;
    result.at(3, 1) = offset.y;
    result.at(3, 2) = offset.z;
    return result;
}

Mat4 Mat4::scaling(Vec3 factors)
{
    auto result = Mat4 {};
    result.at(0, 0) = factors.x;
    result.at(1, 1) = factors.y;
    result.at(2, 2) = factors.z;
    result.at(3, 3) = 1.0f;
    return result;
}

Mat4 Mat4::rotation(Quat q)
{
    auto result = identity();

    auto xx = q.x * q.x;
    auto yy = q.y * q.y;
    auto zz = q.z * q.z;
    auto xy = q.x * q.y;
    auto xz = q.x * q.z;
    auto yz = q.y * q.z;
    auto wx = q.w * q.x;
    auto wy = q.w * q.y;
    auto wz = q.w * q.z;

    result.at(0, 0) = 1.0f - 2.0f * (yy + zz);
    result.at(0, 1) = 2.0f * (xy + wz);
    result.at(0, 2) = 2.0f * (xz - wy);

    result.at(1, 0) = 2.0f * (xy - wz);
    result.at(1, 1) = 1.0f - 2.0f * (xx + zz);
    result.at(1, 2) = 2.0f * (yz + wx);

    result.at(2, 0) = 2.0f * (xz + wy);
    result.at(2, 1) = 2.0f * (yz - wx);
    result.at(2, 2) = 1.0f - 2.0f * (xx + yy);

    return result;
}

Mat4 Mat4::fromTRS(Vec3 translationToUse, Quat rotationToUse, Vec3 scale)
{
    return translation(translationToUse) * rotation(rotationToUse) * scaling(scale);
}

Mat4 Mat4::perspective(float aspect, float fovYRadians, float nearZ, float farZ)
{
    auto result = Mat4 {};
    auto focal = 1.0f / std::tan(fovYRadians * 0.5f);

    result.at(0, 0) = focal / aspect;
    result.at(1, 1) = focal;
    result.at(2, 2) = farZ / (nearZ - farZ);
    result.at(2, 3) = -1.0f;
    result.at(3, 2) = (farZ * nearZ) / (nearZ - farZ);

    return result;
}

Mat4 Mat4::lookAt(Vec3 eye, Vec3 target, Vec3 up)
{
    // Right-handed: the camera looks down its own -z, so the basis vector that
    // goes in column 2 is the direction *away* from what is being looked at.
    auto back = normalize(eye - target);
    auto right = normalize(cross(up, back));
    auto trueUp = cross(back, right);

    auto result = identity();

    result.at(0, 0) = right.x;
    result.at(1, 0) = right.y;
    result.at(2, 0) = right.z;

    result.at(0, 1) = trueUp.x;
    result.at(1, 1) = trueUp.y;
    result.at(2, 1) = trueUp.z;

    result.at(0, 2) = back.x;
    result.at(1, 2) = back.y;
    result.at(2, 2) = back.z;

    result.at(3, 0) = -dot(right, eye);
    result.at(3, 1) = -dot(trueUp, eye);
    result.at(3, 2) = -dot(back, eye);

    return result;
}

Vec3 Mat4::transformPoint(Vec3 point) const
{
    return {at(0, 0) * point.x + at(1, 0) * point.y + at(2, 0) * point.z + at(3, 0),
            at(0, 1) * point.x + at(1, 1) * point.y + at(2, 1) * point.z + at(3, 1),
            at(0, 2) * point.x + at(1, 2) * point.y + at(2, 2) * point.z + at(3, 2)};
}

Vec3 Mat4::transformDirection(Vec3 direction) const
{
    return {at(0, 0) * direction.x + at(1, 0) * direction.y + at(2, 0) * direction.z,
            at(0, 1) * direction.x + at(1, 1) * direction.y + at(2, 1) * direction.z,
            at(0, 2) * direction.x + at(1, 2) * direction.y
                + at(2, 2) * direction.z};
}

float Mat4::linearDeterminant() const
{
    return at(0, 0) * (at(1, 1) * at(2, 2) - at(2, 1) * at(1, 2))
           - at(1, 0) * (at(0, 1) * at(2, 2) - at(2, 1) * at(0, 2))
           + at(2, 0) * (at(0, 1) * at(1, 2) - at(1, 1) * at(0, 2));
}

Mat4 Mat4::normalMatrix() const
{
    auto determinant = linearDeterminant();
    auto result = identity();

    // A singular linear part - a scale of zero on some axis - has no inverse,
    // and a flattened mesh has no meaningful normals either. Falling back to the
    // identity keeps the shading wrong rather than NaN, which is the difference
    // between a model that looks odd and one that disappears.
    if (std::fabs(determinant) < 1.0e-12f)
        return result;

    auto inverseDeterminant = 1.0f / determinant;

    // The cofactor matrix over the determinant is the inverse transpose
    // directly: cofactor(i, j) lands at (i, j) rather than at (j, i), which is
    // the transpose the normal needs, so there is no second flip here.
    for (auto column = 0; column < 3; ++column)
    {
        for (auto row = 0; row < 3; ++row)
        {
            auto c0 = (column + 1) % 3;
            auto c1 = (column + 2) % 3;
            auto r0 = (row + 1) % 3;
            auto r1 = (row + 2) % 3;

            result.at(column, row) =
                (at(c0, r0) * at(c1, r1) - at(c1, r0) * at(c0, r1))
                * inverseDeterminant;
        }
    }

    return result;
}

Mat4 operator*(const Mat4& a, const Mat4& b)
{
    auto result = Mat4 {};

    for (auto column = 0; column < 4; ++column)
        for (auto row = 0; row < 4; ++row)
        {
            auto sum = 0.0f;

            for (auto k = 0; k < 4; ++k)
                sum += a.at(k, row) * b.at(column, k);

            result.at(column, row) = sum;
        }

    return result;
}
} // namespace eacp::Mesh
