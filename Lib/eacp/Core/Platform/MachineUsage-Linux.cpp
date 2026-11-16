#include "MachineUsagePlatform.h"

#include "../Utils/Files.h"
#include "../Utils/Strings.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <sstream>
#include <unistd.h>

namespace eacp::Platform
{
namespace
{
std::string readProcFile(const char* path)
{
    return Files::readFile(FilePath {path});
}

std::uint64_t readMemInfoBytes(std::string_view memInfo, std::string_view field)
{
    constexpr auto bytesPerKibibyte = std::uint64_t {1024};

    auto line = std::istringstream {Strings::lineContaining(memInfo, field)};
    auto name = std::string {};
    auto kibibytes = std::uint64_t {0};
    line >> name >> kibibytes;

    return kibibytes * bytesPerKibibyte;
}

std::uint64_t readResidentProcessBytes()
{
    auto statm = std::istringstream {readProcFile("/proc/self/statm")};
    auto totalPages = std::uint64_t {0};
    auto residentPages = std::uint64_t {0};
    statm >> totalPages >> residentPages;

    const auto pageBytes = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    return residentPages * pageBytes;
}
} // namespace

// The first line of /proc/stat is "cpu" and the ticks spent in user, nice,
// system, idle, iowait, irq, softirq and steal; guest time is inside user.
std::optional<Detail::SystemCpuTicks> Detail::readSystemCpuTicks()
{
    constexpr auto countedFields = 8;
    constexpr auto idleIndex = 3;
    constexpr auto ioWaitIndex = 4;

    auto stat = std::istringstream {readProcFile("/proc/stat")};
    auto label = std::string {};
    auto ticks = std::array<std::uint64_t, countedFields> {};

    stat >> label;

    for (auto& value: ticks)
        stat >> value;

    if (label != "cpu" || !stat)
        return {};

    const auto total =
        std::accumulate(ticks.begin(), ticks.end(), std::uint64_t {0});
    const auto idle = ticks[idleIndex] + ticks[ioWaitIndex];

    return SystemCpuTicks {.busy = total - idle, .total = total};
}

MemoryUsage getMemoryUsage()
{
    const auto memInfo = readProcFile("/proc/meminfo");
    const auto total = readMemInfoBytes(memInfo, "MemTotal:");
    const auto available = readMemInfoBytes(memInfo, "MemAvailable:");

    return {.processBytes = readResidentProcessBytes(),
            .systemUsedBytes = total - std::min(available, total),
            .systemTotalBytes = total};
}
} // namespace eacp::Platform
