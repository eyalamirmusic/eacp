#pragma once

#include <random>

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
        return static_cast<T>(getNext(static_cast<double>(min),
                                      static_cast<double>(max)));
    }

private:
    std::mt19937 engine;
};


}