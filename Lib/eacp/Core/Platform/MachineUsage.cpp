#include "MachineUsage.h"
#include "MachineUsagePlatform.h"

#include <algorithm>
#include <chrono>

namespace eacp::Platform
{
namespace
{
std::int64_t readSteadyClockNanoseconds()
{
    const auto sinceStart = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(sinceStart).count();
}

Detail::CpuTimes readCpuTimes()
{
    return {.wallNanoseconds = readSteadyClockNanoseconds(),
            .processCpuNanoseconds = Detail::readProcessCpuNanoseconds(),
            .system = Detail::readSystemCpuTicks()};
}

double calculateRatio(double numerator, double denominator)
{
    if (denominator <= 0.0)
        return 0.0;

    return numerator / denominator;
}

std::optional<double>
    calculateSystemFraction(const std::optional<Detail::SystemCpuTicks>& earlier,
                            const std::optional<Detail::SystemCpuTicks>& later)
{
    if (!earlier || !later)
        return {};

    const auto busyTicks =
        static_cast<double>(later->busy) - static_cast<double>(earlier->busy);
    const auto totalTicks =
        static_cast<double>(later->total) - static_cast<double>(earlier->total);

    return std::clamp(calculateRatio(busyTicks, totalTicks), 0.0, 1.0);
}
} // namespace

CpuLoad Detail::calculateCpuLoad(const CpuTimes& earlier, const CpuTimes& later)
{
    const auto processNanoseconds = static_cast<double>(
        later.processCpuNanoseconds - earlier.processCpuNanoseconds);
    const auto wallNanoseconds =
        static_cast<double>(later.wallNanoseconds - earlier.wallNanoseconds);

    return {.processCores = calculateRatio(processNanoseconds, wallNanoseconds),
            .systemFraction = calculateSystemFraction(earlier.system, later.system)};
}

CpuLoadMeter::CpuLoadMeter()
    : previous(readCpuTimes())
{
}

CpuLoad CpuLoadMeter::measureSinceLastCall()
{
    const auto current = readCpuTimes();
    const auto load = Detail::calculateCpuLoad(previous, current);
    previous = current;
    return load;
}

} // namespace eacp::Platform
