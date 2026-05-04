#include "Commands.h"

#include <chrono>

namespace
{
long long currentEpochMillis()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}
} // namespace

PingResponse handlePing(const Miro::EmptyValue&)
{
    return PingResponse {.pong = true, .serverTimeMs = currentEpochMillis()};
}

MIRO_EXPORT_COMMAND(ping, handlePing)
