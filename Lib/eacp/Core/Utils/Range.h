#pragma once

namespace eacp
{
// A half-open interval [start, start + length). It stores `start` and
// `length` -- never an `end` member -- so a default-constructed Range is
// empty and a zero length means empty regardless of where it starts. `end()`
// is derived on demand.
template <typename T>
struct Range
{
    constexpr bool empty() const { return length == T {}; }

    // One-past-the-last element. Derived, never stored.
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
