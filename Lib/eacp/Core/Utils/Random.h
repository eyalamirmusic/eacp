#pragma once

/** @file Seedable pseudo-random number generator. */

#include "Pimpl.h"

namespace eacp
{
/// Uniform random numbers from a std::mt19937. Not thread-safe; one per thread.
class Random
{
public:
    /// Seeded from std::random_device.
    Random();
    Random(unsigned int seed);

    /// Uniform in [min, max).
    double getNext(double min, double max);

    /// getNext cast to T; for integers max itself is never returned.
    template <typename T>
    T get(T min, T max)
    {
        return static_cast<T>(
            getNext(static_cast<double>(min), static_cast<double>(max)));
    }

private:
    struct Impl;
    Pimpl<Impl> impl;
};

} // namespace eacp
