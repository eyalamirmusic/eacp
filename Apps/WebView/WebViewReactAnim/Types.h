#pragma once

#include <Miro/Bridge.h>
#include <Miro/Reflect.h>

#include <chrono>
#include <cmath>

// File scope keeps qualifiedName matching the generated TS wire format.
struct Tick
{
    double angle = 0.0;

    MIRO_REFLECT(angle)
};

namespace Api
{
using namespace std::chrono;

class Clock
{
public:
    void reflect(Miro::ApiReflector& r)
    {
        using T = Clock;

        r.commands<&T::getCurrentTick>();
        r.events<&T::tick>();
    }

    Tick getCurrentTick() const
    {
        auto seconds = duration<double>(steady_clock::now() - startTime).count();
        return {.angle = std::fmod(seconds * 90.0, 360.0)};
    }

    void update() { tick.publish(getCurrentTick()); }

    Miro::Event<Tick> tick;

private:
    steady_clock::time_point startTime = steady_clock::now();
};

} // namespace Api
