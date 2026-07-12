#pragma once

#include "Time.h"

#include <chrono>

// The Time::MS -> std::chrono boundary, for implementation files that do real
// time arithmetic. Public headers only see Time::MS.
namespace eacp::Time
{
inline std::chrono::milliseconds toStdChrono(MS duration)
{
    return std::chrono::milliseconds {duration.count};
}

inline MS toMS(std::chrono::milliseconds duration)
{
    return MS {duration.count()};
}
} // namespace eacp::Time
