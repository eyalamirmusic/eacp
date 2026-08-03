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

// Same nesting as WebViewSubApi — CounterApi is mounted under `nested`, so it
// reaches the wire as "nested.getCounter" / "nested.counter" while the client
// generated from it is written against the bare names. What differs is the
// page: this one is React and consumes the GENERATED hooks module, so its
// subscriptions are set up by eacp's own codegen rather than by hand.
//
// The two events are chosen to cover both hook shapes HooksFormat.cpp picks
// between, because they subscribe at different times:
//
//   counter — has a matching getCounter command, so it becomes a
//             makeBridgeStore. That factory runs its fetch and its
//             backend.on() in its own body, which the generated hooks module
//             calls at MODULE SCOPE.
//   pulse   — has no matching getter, so it becomes a makeNativeEvent, which
//             subscribes inside a useEffect — i.e. after render.
class CounterApi
{
public:
    // Seeded away from Counter{} so a rendered initial fetch is
    // distinguishable from the hook's generated initial value, which comes
    // from toJSON(Counter{}) and is 0.
    static constexpr auto seededCounter = 42;

    Counter getCounter() const { return counter.snapshot(); }

    // Driven from C++ so the subscribe path is covered independently of the
    // fetch path.
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
