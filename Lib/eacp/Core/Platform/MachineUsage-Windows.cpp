#include "MachineUsagePlatform.h"

#include "../Utils/WinInclude.h"

#include <psapi.h>

namespace eacp::Platform
{
namespace
{
std::uint64_t readFileTimeTicks(const FILETIME& time)
{
    constexpr auto bitsInLowDateTime = 32;

    const auto high = static_cast<std::uint64_t>(time.dwHighDateTime);
    return (high << bitsInLowDateTime) | time.dwLowDateTime;
}

// Task Manager's "Memory" column.
std::uint64_t readPrivateWorkingSetBytes()
{
    auto counters = PROCESS_MEMORY_COUNTERS_EX2 {};
    auto* countersOut = reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters);

    if (!GetProcessMemoryInfo(GetCurrentProcess(), countersOut, sizeof(counters)))
        return 0;

    return counters.PrivateWorkingSetSize;
}
} // namespace

std::int64_t Detail::readProcessCpuNanoseconds()
{
    constexpr auto nanosecondsPerFileTimeTick = std::int64_t {100};

    auto creation = FILETIME {};
    auto exit = FILETIME {};
    auto kernel = FILETIME {};
    auto user = FILETIME {};

    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return 0;

    const auto ticks = static_cast<std::int64_t>(readFileTimeTicks(kernel)
                                                 + readFileTimeTicks(user));
    return ticks * nanosecondsPerFileTimeTick;
}

// Kernel time includes idle time.
std::optional<Detail::SystemCpuTicks> Detail::readSystemCpuTicks()
{
    auto idle = FILETIME {};
    auto kernel = FILETIME {};
    auto user = FILETIME {};

    if (!GetSystemTimes(&idle, &kernel, &user))
        return {};

    const auto total = readFileTimeTicks(kernel) + readFileTimeTicks(user);
    return SystemCpuTicks {.busy = total - readFileTimeTicks(idle), .total = total};
}

MemoryUsage getMemoryUsage()
{
    auto status = MEMORYSTATUSEX {};
    status.dwLength = sizeof(status);
    auto usage = MemoryUsage {.processBytes = readPrivateWorkingSetBytes()};

    if (GlobalMemoryStatusEx(&status))
    {
        usage.systemTotalBytes = status.ullTotalPhys;
        usage.systemUsedBytes = status.ullTotalPhys - status.ullAvailPhys;
    }

    return usage;
}
} // namespace eacp::Platform
