#pragma once

#include <compare>
#include <cstdint>

namespace eacp::Time
{
// A duration in milliseconds. The framework's time currency: public APIs take
// this instead of std::chrono types so headers stay free of <chrono> (the
// same boundary FilePath draws around <filesystem>; see StdChrono.h).
struct MS
{
    std::int64_t count = 0;

    friend constexpr auto operator<=>(MS, MS) = default;
};

// A point in the future, for pump-until loops. Wraps the steady clock behind
// out-of-line methods so headers using it stay free of <chrono>.
class Deadline
{
public:
    explicit Deadline(MS timeout);

    bool expired() const;

    // Time left until the deadline, clamped to zero.
    MS remaining() const;

private:
    std::int64_t end = 0;
};
} // namespace eacp::Time
