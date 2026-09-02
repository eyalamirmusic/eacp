#pragma once

#include <concepts>

namespace eacp::Maths
{
namespace detail
{
// Kept at the widest precision the platform has, so the conversions below give
// a double its own digits rather than a float's. `pi` itself stays a float,
// which is what every call site here wants.
inline constexpr auto exactPi = 3.14159265358979323846264338327950288L;
} // namespace detail

constexpr auto pi = (float) detail::exactPi;
constexpr auto twoPi = 2.f * pi;
constexpr auto halfPi = 0.5f * pi;

// The length below which a vector counts as having no direction, so normalize()
// answers with a zero vector instead of dividing by it and handing back NaNs.
constexpr auto epsilon = 1.e-6f;

// How near two floats have to be for nearlyEqual to call them equal. Wider than
// epsilon on purpose: this is the rounding a few operations leave behind — a
// matrix product, a normalize, a trig call — rather than a threshold for a value
// being nothing at all.
constexpr auto tolerance = 1.e-5f;

template <std::floating_point T>
constexpr T radians(T degreeValue)
{
    return degreeValue * (T) (detail::exactPi / 180.L);
}

template <std::floating_point T>
constexpr T degrees(T radianValue)
{
    return radianValue * (T) (180.L / detail::exactPi);
}

// Float equality with slack, which is the only kind worth asking for once a
// value has been through any arithmetic at all. A NaN on either side is never
// nearly equal to anything, itself included.
template <std::floating_point T>
constexpr bool nearlyEqual(T a, T b, T allowed = (T) tolerance)
{
    return (a > b ? a - b : b - a) <= allowed;
}
} // namespace eacp::Maths
