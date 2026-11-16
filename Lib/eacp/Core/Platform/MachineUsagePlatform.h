#pragma once

#include "MachineUsage.h"

namespace eacp::Platform::Detail
{
struct SystemCpuTicks
{
    std::uint64_t busy = 0;
    std::uint64_t total = 0;
};

struct CpuTimes
{
    std::int64_t wallNanoseconds = 0;
    std::int64_t processCpuNanoseconds = 0;
    std::optional<SystemCpuTicks> system;
};

// A 32-bit counter that wraps, widened to 64 bits: the widened value so far
// plus how far the counter moved since its previous reading, modulo 2^32.
std::uint64_t advanceWrappingCounter(std::uint64_t widened,
                                     std::uint32_t previous,
                                     std::uint32_t current);

std::int64_t readProcessCpuNanoseconds();

// Empty where the system will not say.
std::optional<SystemCpuTicks> readSystemCpuTicks();

CpuLoad calculateCpuLoad(const CpuTimes& earlier, const CpuTimes& later);
} // namespace eacp::Platform::Detail
