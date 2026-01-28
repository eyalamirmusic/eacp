#include "Random.h"
#include "Singleton.h"

namespace eacp
{
Random::Random()
    : engine(std::random_device()()) {}

Random::Random(unsigned int seed)
    : engine(seed) {}

double Random::getNext(double min, double max)
{
    auto dist = std::uniform_real_distribution(min, max);
    return dist(engine);
}

Random& getGlobalRandom()
{
    return Singleton::get<Random>();
}
}