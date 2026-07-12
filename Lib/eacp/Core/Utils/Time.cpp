#include "Time.h"

#include <chrono>

namespace eacp::Time
{
namespace
{
std::int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

Deadline::Deadline(MS timeout)
    : end(nowMs() + timeout.count)
{
}

bool Deadline::expired() const
{
    return nowMs() >= end;
}

MS Deadline::remaining() const
{
    auto left = end - nowMs();
    return MS {left > 0 ? left : 0};
}
} // namespace eacp::Time
