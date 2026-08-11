#pragma once

#include <compare>
#include <cstdint>

namespace eacp::Time
{
// The framework's duration type, so public headers stay free of <chrono>.
struct MS
{
    std::int64_t count = 0;

    friend constexpr auto operator<=>(MS, MS) = default;
};

void sleep(MS duration);

inline void sleepMS(int ms)
{
    sleep(MS {ms});
}

// A point on the steady clock, for pump-until loops.
class Deadline
{
public:
    explicit Deadline(MS timeout);

    bool expired() const;

    // Clamped to zero once expired.
    MS remaining() const;

private:
    std::int64_t end = 0;
};
} // namespace eacp::Time
