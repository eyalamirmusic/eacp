#pragma once

#include "../Utils/Pimpl.h"

#include <cstdint>
#include <optional>

namespace eacp::Platform
{

// A figure the system would not give is 0.
struct MemoryUsage
{
    // What Activity Monitor, Task Manager or top shows for this process.
    std::uint64_t processBytes = 0;
    // The total less what the system could hand out without swapping.
    std::uint64_t systemUsedBytes = 0;
    std::uint64_t systemTotalBytes = 0;
};

MemoryUsage getMemoryUsage();

struct CpuLoad
{
    // 2.5 is this process keeping two and a half cores busy.
    double processCores = 0.0;
    // The busy share of the whole machine, empty where an app may not read it
    // (Android since 8).
    std::optional<double> systemFraction;
};

// The load between two calls, the first measured from construction. Windows
// counts CPU time in 15.6 ms scheduler ticks, so calls closer together than
// that read in whole ticks there.
class CpuLoadMeter
{
public:
    CpuLoadMeter();

    CpuLoad measureSinceLastCall();

private:
    struct Impl;
    Pimpl<Impl> impl;
};

} // namespace eacp::Platform
