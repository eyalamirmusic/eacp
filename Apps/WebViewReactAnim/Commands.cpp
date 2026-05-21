#include "Types.h"

#include <chrono>
#include <cmath>

namespace
{
using Clock = std::chrono::steady_clock;
const auto start = Clock::now();
} // namespace

Tick getCurrentTick()
{
    auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return {.angle = std::fmod(seconds * 90.0, 360.0)};
}

MIRO_EXPORT_COMMAND(getCurrentTick)

MIRO_EXPORT_EVENT(tick, Tick)
