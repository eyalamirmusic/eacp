#pragma once

#include "Pimpl.h"

namespace eacp
{
class Random
{
public:
    Random();
    Random(unsigned int seed);

    double getNext(double min, double max);

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
