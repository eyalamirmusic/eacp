#pragma once

namespace eacp
{
// A half-open interval [start, start + length). Stored as start + length (no
// `end` member), so a default-constructed or zero-length Range is empty
// regardless of start.
template <typename T>
struct Range
{
    constexpr bool empty() const { return length == T {}; }

    // One-past-the-last element.
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
