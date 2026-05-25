#pragma once

#include <Miro/Miro.h>

#include <chrono>
#include <cmath>

// Tick stays at file scope (not in an Api:: namespace) so its
// qualifiedName matches the baseline TS output exactly — switching the
// codegen path mustn't change the wire format.
struct Tick
{
    double angle = 0.0;

    MIRO_REFLECT(angle)
};

namespace Api
{
using namespace std::chrono;
// The whole WebViewReactAnim API. The `reflect` method is the single
// source of truth for codegen (DescribeReflector walks it) and runtime
// (BindReflector walks the same body to install handlers + subscribe
// to the tick event). Replaces the MIRO_EXPORT_COMMAND(getCurrentTick)
// + EACP_EVENT(tick, Tick) pair the static-init path used.
class Clock
{
public:
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
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

    // Push channel for tick updates. The MyApp timer publishes here;
    // the transport's BindReflector listener forwards each payload over
    // the WebView bridge.
    Miro::Event<Tick> tick;

private:
    steady_clock::time_point startTime = steady_clock::now();
};

} // namespace Api
