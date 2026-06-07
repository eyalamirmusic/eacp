#pragma once

#include <array>
#include <cmath>

namespace teapot
{
// Column-major 4x4 matrix matching Metal's float4x4 memory layout: element
// (row, col) lives at data[col * 4 + row]. operator* composes in standard math
// order (a * b applies b, then a), and `data` uploads straight into a
// Uniform<Float4x4>.
struct Mat4
{
    std::array<float, 16> data {};

    static Mat4 identity()
    {
        auto m = Mat4 {};
        m.data[0] = m.data[5] = m.data[10] = m.data[15] = 1.0f;
        return m;
    }

    static Mat4 translation(float x, float y, float z)
    {
        auto m = identity();
        m.data[12] = x;
        m.data[13] = y;
        m.data[14] = z;
        return m;
    }

    static Mat4 rotationX(float radians)
    {
        auto c = std::cos(radians);
        auto s = std::sin(radians);
        auto m = identity();
        m.data[5] = c;
        m.data[6] = s;
        m.data[9] = -s;
        m.data[10] = c;
        return m;
    }

    static Mat4 rotationZ(float radians)
    {
        auto c = std::cos(radians);
        auto s = std::sin(radians);
        auto m = identity();
        m.data[0] = c;
        m.data[1] = s;
        m.data[4] = -s;
        m.data[5] = c;
        return m;
    }

    // Right-handed perspective with depth in [0, 1] (Metal / D3D convention).
    static Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ)
    {
        auto f = 1.0f / std::tan(fovYRadians * 0.5f);
        auto m = Mat4 {};
        m.data[0] = f / aspect;
        m.data[5] = f;
        m.data[10] = farZ / (nearZ - farZ);
        m.data[11] = -1.0f;
        m.data[14] = (farZ * nearZ) / (nearZ - farZ);
        return m;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b)
{
    auto result = Mat4 {};

    for (auto col = 0; col < 4; ++col)
        for (auto row = 0; row < 4; ++row)
        {
            auto sum = 0.0f;

            for (auto k = 0; k < 4; ++k)
                sum += a.data[k * 4 + row] * b.data[col * 4 + k];

            result.data[col * 4 + row] = sum;
        }

    return result;
}
} // namespace teapot
