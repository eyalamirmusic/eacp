#include "Random.h"
#include "Singleton.h"

#include <random>

namespace eacp
{
struct Random::Impl
{
    explicit Impl(unsigned int seed)
        : engine(seed)
    {
    }

    std::mt19937 engine;
};

Random::Random()
    : impl(std::random_device()())
{
}

Random::Random(unsigned int seed)
    : impl(seed)
{
}

double Random::getNext(double min, double max)
{
    auto dist = std::uniform_real_distribution(min, max);
    return dist(impl->engine);
}

Random& getGlobalRandom()
{
    return Singleton::get<Random>();
}
} // namespace eacp
