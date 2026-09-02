#pragma once

#include "../Utils/Containers.h"
#include "Vec.h"

#include <utility>

namespace eacp::Maths
{
// A 4x4 transform, stored column-major — values[column * 4 + row] — which is the
// layout Metal and HLSL both give a float4x4, so one of these uploads into a
// GPU::Uniform<Float4x4> byte for byte with nothing to transpose.
//
// The projections are right-handed with a [0, 1] depth range: the camera looks
// down -z, and the near plane maps to 0 rather than to -1. That is what Metal
// and D3D12 clip against, and what ShaderProgram's own perspective() builds, so
// a matrix assembled here and one assembled inside define() agree.
//
// A default-constructed Mat4 is the identity, not a block of zeros — the useful
// neutral value, and the one a transform left unset should behave as.
struct Mat4
{
    static Mat4 translation(const Vec3& offset);
    static Mat4 scale(const Vec3& factors);
    static Mat4 scale(float factor);

    static Mat4 rotationX(float radians);
    static Mat4 rotationY(float radians);
    static Mat4 rotationZ(float radians);

    static Mat4 perspective(float aspect, float fovY, float nearZ, float farZ);

    static Mat4 orthographic(
        float left, float right, float bottom, float top, float nearZ, float farZ);

    // The view matrix for a camera at eye looking at target. `up` only has to
    // point roughly upward; the part of it along the view direction is removed.
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up);

    float& at(int column, int row) { return values[column * 4 + row]; }

    const float& at(int column, int row) const { return values[column * 4 + row]; }

    Vec4 column(int index) const
    {
        return {at(index, 0), at(index, 1), at(index, 2), at(index, 3)};
    }

    void setColumn(int index, const Vec4& value)
    {
        for (auto row = 0; row < 4; ++row)
            at(index, row) = value[row];
    }

    Mat4 transposed() const;

    // A singular matrix has no inverse, and the identity comes back for one, so
    // the result stays usable rather than turning into a block of NaNs.
    Mat4 inverted() const;

    const float* data() const { return values.data(); }

    bool operator==(const Mat4&) const = default;

    // clang-format off
    Array<float, 16> values {1.f, 0.f, 0.f, 0.f,
                             0.f, 1.f, 0.f, 0.f,
                             0.f, 0.f, 1.f, 0.f,
                             0.f, 0.f, 0.f, 1.f};
    // clang-format on
};

inline Mat4 Mat4::translation(const Vec3& offset)
{
    auto result = Mat4 {};
    result.setColumn(3, {offset.x, offset.y, offset.z, 1.f});

    return result;
}

inline Mat4 Mat4::scale(const Vec3& factors)
{
    auto result = Mat4 {};

    for (auto axis = 0; axis < 3; ++axis)
        result.at(axis, axis) = factors[axis];

    return result;
}

inline Mat4 Mat4::scale(float factor)
{
    return scale(Vec3 {factor, factor, factor});
}

inline Mat4 Mat4::rotationX(float radians)
{
    auto c = std::cos(radians);
    auto s = std::sin(radians);

    auto result = Mat4 {};
    result.setColumn(1, {0.f, c, s, 0.f});
    result.setColumn(2, {0.f, -s, c, 0.f});

    return result;
}

inline Mat4 Mat4::rotationY(float radians)
{
    auto c = std::cos(radians);
    auto s = std::sin(radians);

    auto result = Mat4 {};
    result.setColumn(0, {c, 0.f, -s, 0.f});
    result.setColumn(2, {s, 0.f, c, 0.f});

    return result;
}

inline Mat4 Mat4::rotationZ(float radians)
{
    auto c = std::cos(radians);
    auto s = std::sin(radians);

    auto result = Mat4 {};
    result.setColumn(0, {c, s, 0.f, 0.f});
    result.setColumn(1, {-s, c, 0.f, 0.f});

    return result;
}

inline Mat4 Mat4::perspective(float aspect, float fovY, float nearZ, float farZ)
{
    auto focal = 1.f / std::tan(fovY * 0.5f);

    auto result = Mat4 {};
    result.setColumn(0, {focal / aspect, 0.f, 0.f, 0.f});
    result.setColumn(1, {0.f, focal, 0.f, 0.f});
    result.setColumn(2, {0.f, 0.f, farZ / (nearZ - farZ), -1.f});
    result.setColumn(3, {0.f, 0.f, (farZ * nearZ) / (nearZ - farZ), 0.f});

    return result;
}

inline Mat4 Mat4::orthographic(
    float left, float right, float bottom, float top, float nearZ, float farZ)
{
    auto width = right - left;
    auto height = top - bottom;
    auto depth = nearZ - farZ;

    auto result = Mat4 {};
    result.setColumn(0, {2.f / width, 0.f, 0.f, 0.f});
    result.setColumn(1, {0.f, 2.f / height, 0.f, 0.f});
    result.setColumn(2, {0.f, 0.f, 1.f / depth, 0.f});
    result.setColumn(
        3, {-(right + left) / width, -(top + bottom) / height, nearZ / depth, 1.f});

    return result;
}

inline Mat4 Mat4::lookAt(const Vec3& eye, const Vec3& target, const Vec3& up)
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

inline Mat4 Mat4::transposed() const
{
    auto result = Mat4 {};

    for (auto column = 0; column < 4; ++column)
        for (auto row = 0; row < 4; ++row)
            result.at(row, column) = at(column, row);

    return result;
}

namespace detail
{
inline void swapRows(Mat4& matrix, int a, int b)
{
    for (auto column = 0; column < 4; ++column)
        std::swap(matrix.at(column, a), matrix.at(column, b));
}
} // namespace detail

// Gauss-Jordan with partial pivoting: `working` is driven to the identity and
// the same operations, applied to a matrix that starts as the identity, leave
// the inverse behind.
inline Mat4 Mat4::inverted() const
{
    auto working = *this;
    auto result = Mat4 {};

    for (auto pivot = 0; pivot < 4; ++pivot)
    {
        auto best = pivot;

        for (auto row = pivot + 1; row < 4; ++row)
            if (std::abs(working.at(pivot, row)) > std::abs(working.at(pivot, best)))
                best = row;

        if (std::abs(working.at(pivot, best)) < epsilon)
            return {};

        detail::swapRows(working, pivot, best);
        detail::swapRows(result, pivot, best);

        auto inversePivot = 1.f / working.at(pivot, pivot);

        for (auto column = 0; column < 4; ++column)
        {
            working.at(column, pivot) *= inversePivot;
            result.at(column, pivot) *= inversePivot;
        }

        for (auto row = 0; row < 4; ++row)
        {
            if (row == pivot)
                continue;

            auto factor = working.at(pivot, row);

            for (auto column = 0; column < 4; ++column)
            {
                working.at(column, row) -= factor * working.at(column, pivot);
                result.at(column, row) -= factor * result.at(column, pivot);
            }
        }
    }

    return result;
}

inline Mat4 operator*(const Mat4& a, const Mat4& b)
{
    auto result = Mat4 {};

    for (auto column = 0; column < 4; ++column)
        for (auto row = 0; row < 4; ++row)
        {
            auto sum = 0.f;

            for (auto k = 0; k < 4; ++k)
                sum += a.at(k, row) * b.at(column, k);

            result.at(column, row) = sum;
        }

    return result;
}

inline Mat4& operator*=(Mat4& a, const Mat4& b)
{
    return a = a * b;
}

inline Vec4 operator*(const Mat4& matrix, const Vec4& vector)
{
    auto result = Vec4 {};

    for (auto row = 0; row < 4; ++row)
    {
        auto sum = 0.f;

        for (auto column = 0; column < 4; ++column)
            sum += matrix.at(column, row) * vector[column];

        result[row] = sum;
    }

    return result;
}

// A position: translated, and divided through by w so a projection matrix gives
// back the point it lands on rather than a homogeneous one.
inline Vec3 transformPoint(const Mat4& matrix, const Vec3& point)
{
    auto transformed = matrix * Vec4 {point.x, point.y, point.z, 1.f};

    if (transformed.w == 0.f)
        return transformed.xyz();

    return transformed.xyz() / transformed.w;
}

// A direction: rotated and scaled, never translated. A normal wants the inverse
// transpose instead whenever the transform scales non-uniformly.
inline Vec3 transformDirection(const Mat4& matrix, const Vec3& direction)
{
    auto transformed = matrix * Vec4 {direction.x, direction.y, direction.z, 0.f};
    return transformed.xyz();
}

// What to compare two matrices with: exact equality holds for one that was
// assembled rather than computed, and stops holding the moment a product or an
// inverse has rounded anything.
inline bool nearlyEqual(const Mat4& a, const Mat4& b, float allowed = tolerance)
{
    for (auto column = 0; column < 4; ++column)
        for (auto row = 0; row < 4; ++row)
            if (!nearlyEqual(a.at(column, row), b.at(column, row), allowed))
                return false;

    return true;
}
} // namespace eacp::Maths
