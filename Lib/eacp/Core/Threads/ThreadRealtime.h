#pragma once

#include "ThreadUtils.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>

namespace eacp::Threads::Detail
{
struct RealtimeConstraint
{
    double periodMs = 0.0;
    double computationMs = 0.0;
    double constraintMs = 0.0;
};

// JUCE's derivation: computation from the processing time, else the maximum,
// else the period, else 10 ms; the constraint from the maximum, else computation.
inline RealtimeConstraint realtimeConstraintFor(const RealtimeOptions& options)
{
    auto firstSet = [](std::initializer_list<double> values, double fallback)
    {
        for (auto value: values)
            if (value > 0.0)
                return value;

        return fallback;
    };

    auto computation = firstSet(
        {options.processingTimeMs, options.maxProcessingTimeMs, options.periodMs},
        10.0);

    return {options.periodMs > 0.0 ? options.periodMs : 0.0,
            computation,
            firstSet({options.maxProcessingTimeMs}, computation)};
}

inline std::uint32_t machTicksFromMilliseconds(double milliseconds,
                                               std::uint32_t numer,
                                               std::uint32_t denom)
{
    auto nanoseconds = milliseconds * 1'000'000.0;
    return static_cast<std::uint32_t>(nanoseconds * denom / numer);
}

// JUCE's default realtime level, 5 on its 0..10 scale, mapped onto the range.
inline int realtimeSchedulerPriority(int minimum, int maximum)
{
    auto low = std::max(0, minimum);
    auto high = std::max(1, maximum);
    return low + 5 * (high - low) / 10;
}
} // namespace eacp::Threads::Detail
