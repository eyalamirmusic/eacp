#include "StopRequest.h"

#include <atomic>
#include <csignal>

namespace StopRequest
{
namespace
{
static_assert(std::atomic<bool>::is_always_lock_free);

std::atomic<bool> requested {false};

void recordRequest(int)
{
    requested.store(true);
}
} // namespace

void installHandler()
{
    std::signal(SIGTERM, recordRequest);
}

bool wasRequested()
{
    return requested.load();
}
} // namespace StopRequest
