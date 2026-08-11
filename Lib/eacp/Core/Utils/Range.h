#pragma once

namespace eacp
{
// A half-open interval [start, start + length).
template <typename T>
struct Range
{
    constexpr bool empty() const { return length == T {}; }

    constexpr T end() const { return start + length; }

    constexpr bool contains(T value) const
    {
        return value >= start && value < end();
    }

    constexpr bool operator==(const Range& other) const = default;

    T start {};
    T length {};
};
} // namespace eacp
