#pragma once

#include <Miro/Bridge.h>
#include <Miro/Reflect.h>

struct Counter
{
    int value = 0;

    MIRO_REFLECT(value)
};

struct Pulse
{
    int beat = 0;

    MIRO_REFLECT(beat)
};

namespace Api
{

// Mounted under `nested`, so the wire names gain that prefix. The two events
// cover both generated hook shapes: counter has a getter, so it becomes a
// module-scope makeBridgeStore; pulse a makeNativeEvent bound in a useEffect.
class CounterApi
{
public:
    // Away from Counter{} so a rendered fetch is distinguishable from the
    // hook's generated initial value of 0.
    static constexpr auto seededCounter = 42;

    Counter getCounter() const { return counter.snapshot(); }

    // Lets a test drive the subscribe path independently of the fetch path.
    void publishCounter(int value) { counter.publish(Counter {value}); }

    void publishPulse(int beat) { pulse.publish(Pulse {beat}); }

    Miro::Event<Counter> counter {Counter {seededCounter}};
    Miro::Event<Pulse> pulse;

    MIRO_REFLECT_API(getCounter, counter, pulse)
};

class RootApi
{
public:
    CounterApi nested;

    MIRO_REFLECT_API(nested)
};

} // namespace Api
