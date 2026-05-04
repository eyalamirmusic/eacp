#include "Types.h"
#include <chrono>

namespace
{
long long currentEpochMillis()
{
    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    return duration_cast<milliseconds>(now).count();
}
} // namespace

PingResponse ping()
{
    return {.pong = true, .serverTimeMs = currentEpochMillis()};
}

MIRO_EXPORT_COMMAND(ping)
