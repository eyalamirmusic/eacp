#include "MachineUsagePlatform.h"

#include <ctime>

namespace eacp::Platform
{
std::int64_t Detail::readProcessCpuNanoseconds()
{
    constexpr auto nanosecondsPerSecond = std::int64_t {1'000'000'000};

    auto time = timespec {};

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0)
        return 0;

    const auto seconds = static_cast<std::int64_t>(time.tv_sec);
    return seconds * nanosecondsPerSecond + time.tv_nsec;
}
} // namespace eacp::Platform
