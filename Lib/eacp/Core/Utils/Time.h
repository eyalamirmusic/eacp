#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace eacp::Time
{
// A duration in milliseconds. The framework's time currency: public APIs take
// this instead of std::chrono types so headers stay free of <chrono> (the
// same boundary FilePath draws around <filesystem>).
struct MS
{
    std::int64_t count = 0;

    friend constexpr auto operator<=>(MS, MS) = default;
};

// Blocks the calling thread, keeping <thread> and <chrono> out of callers
// that just need to sleep.
void sleep(MS duration);

void sleepMS(int ms);

// Wall-clock time since the Unix epoch, for timestamps that leave the process.
MS sinceEpoch();

// UTC as ISO 8601 with milliseconds, "2026-10-27T18:48:02.569Z".
std::string formatIso8601(MS sinceUnixEpoch);

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
