#pragma once

#include "Constants.h"

#include <cmath>
#include <concepts>

namespace eacp::Maths
{
// Plain float vectors: named components, no padding, nothing virtual, so one is
// laid out exactly as the float2 / float3 / float4 a shader reads. That is what
// lets the same struct be the CPU value you do arithmetic on and the vertex
// field or uniform you hand the GPU — see GPU/Codegen/MathValues.h, which
// registers the shader shape of each.
//
// Graphics::Point stays what it is: the y-down point the 2D layer measures a
// window in. Vec2 is the geometry sibling of Vec3 and Vec4, with no orientation
// of its own.
//
// operator[] is what the shared arithmetic below is written against, so each
// operator is defined once for all three rather than three times over.
struct Vec2
{
    static constexpr auto size = 2;

    constexpr float& operator[](int index) { return index == 0 ? x : y; }

    constexpr const float& operator[](int index) const { return index == 0 ? x : y; }

    bool operator==(const Vec2&) const = default;

    float x = 0.f;
    float y = 0.f;
};

struct Vec3
{
    static constexpr auto size = 3;

    constexpr float& operator[](int index)
    {
        return index == 0 ? x : index == 1 ? y : z;
    }

    constexpr const float& operator[](int index) const
    {
        return index == 0 ? x : index == 1 ? y : z;
    }

    constexpr Vec2 xy() const { return {x, y}; }

    bool operator==(const Vec3&) const = default;

    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct Vec4
{
    static constexpr auto size = 4;

    constexpr float& operator[](int index)
    {
        return index == 0 ? x : index == 1 ? y : index == 2 ? z : w;
    }

    constexpr const float& operator[](int index) const
    {
        return index == 0 ? x : index == 1 ? y : index == 2 ? z : w;
    }

    constexpr Vec2 xy() const { return {x, y}; }
    constexpr Vec3 xyz() const { return {x, y, z}; }

    bool operator==(const Vec4&) const = default;

    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float w = 0.f;
};

template <typename T>
concept AnyVec =
    std::same_as<T, Vec2> || std::same_as<T, Vec3> || std::same_as<T, Vec4>;

namespace detail
{
template <AnyVec V, typename Op>
constexpr V zip(const V& a, const V& b, Op op)
{
    auto result = V {};

    for (auto i = 0; i < V::size; ++i)
        result[i] = op(a[i], b[i]);

    return result;
}

template <AnyVec V, typename Op>
constexpr V map(const V& value, Op op)
{
    auto result = V {};

    for (auto i = 0; i < V::size; ++i)
        result[i] = op(value[i]);

    return result;
}
} // namespace detail

template <AnyVec V>
constexpr V operator+(const V& a, const V& b)
{
    return detail::zip(a, b, [](float x, float y) { return x + y; });
}

template <AnyVec V>
constexpr V operator-(const V& a, const V& b)
{
    return detail::zip(a, b, [](float x, float y) { return x - y; });
}

// Component-wise, the way a shader multiplies two vectors — a colour scaled by a
// tint. The products with a geometric meaning are dot() and cross() below.
template <AnyVec V>
constexpr V operator*(const V& a, const V& b)
{
    return detail::zip(a, b, [](float x, float y) { return x * y; });
}

template <AnyVec V>
constexpr V operator/(const V& a, const V& b)
{
    return detail::zip(a, b, [](float x, float y) { return x / y; });
}

template <AnyVec V>
constexpr V operator-(const V& value)
{
    return detail::map(value, [](float v) { return -v; });
}

template <AnyVec V>
constexpr V operator*(const V& value, float scale)
{
    return detail::map(value, [scale](float v) { return v * scale; });
}

template <AnyVec V>
constexpr V operator*(float scale, const V& value)
{
    return value * scale;
}

template <AnyVec V>
constexpr V operator/(const V& value, float divisor)
{
    return detail::map(value, [divisor](float v) { return v / divisor; });
}

template <AnyVec V>
constexpr V& operator+=(V& value, const V& other)
{
    return value = value + other;
}

template <AnyVec V>
constexpr V& operator-=(V& value, const V& other)
{
    return value = value - other;
}

template <AnyVec V>
constexpr V& operator*=(V& value, float scale)
{
    return value = value * scale;
}

template <AnyVec V>
constexpr V& operator/=(V& value, float divisor)
{
    return value = value / divisor;
}

template <AnyVec V>
constexpr float dot(const V& a, const V& b)
{
    auto sum = 0.f;

    for (auto i = 0; i < V::size; ++i)
        sum += a[i] * b[i];

    return sum;
}

constexpr Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

template <AnyVec V>
constexpr float lengthSquared(const V& value)
{
    return dot(value, value);
}

template <AnyVec V>
float length(const V& value)
{
    return std::sqrt(lengthSquared(value));
}

template <AnyVec V>
float distance(const V& a, const V& b)
{
    return length(a - b);
}

// A vector shorter than epsilon has no direction worth keeping, so it comes back
// as the zero vector rather than as NaNs.
template <AnyVec V>
V normalize(const V& value)
{
    auto len = length(value);
    return len > epsilon ? value * (1.f / len) : V {};
}

template <AnyVec V>
constexpr V min(const V& a, const V& b)
{
    return detail::zip(a, b, [](float x, float y) { return x < y ? x : y; });
}

template <AnyVec V>
constexpr V max(const V& a, const V& b)
{
    return detail::zip(a, b, [](float x, float y) { return x > y ? x : y; });
}

template <AnyVec V>
constexpr V lerp(const V& from, const V& to, float amount)
{
    return from + (to - from) * amount;
}

template <AnyVec V>
constexpr bool nearlyEqual(const V& a, const V& b, float allowed = tolerance)
{
    for (auto i = 0; i < V::size; ++i)
        if (!nearlyEqual(a[i], b[i], allowed))
            return false;

    return true;
}
} // namespace eacp::Maths
