#pragma once

#include "MachineUsage.h"

namespace eacp::Platform::Detail
{
std::int64_t readProcessCpuNanoseconds();

// Empty where the system will not say.
std::optional<SystemCpuTicks> readSystemCpuTicks();

CpuLoad calculateCpuLoad(const CpuTimes& earlier, const CpuTimes& later);
} // namespace eacp::Platform::Detail
