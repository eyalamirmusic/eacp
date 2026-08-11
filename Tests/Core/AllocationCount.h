#pragma once

#include <atomic>
#include <cstddef>

// Unlike timing, an allocation count does not depend on the machine or build.
// The global operator new lives in AllocationCount.cpp, replaced exactly once.
extern std::atomic<bool> countingAllocations;
extern std::atomic<std::size_t> allocatedBytes;

struct AllocationCount
{
    AllocationCount()
    {
        allocatedBytes.store(0, std::memory_order_relaxed);
        countingAllocations.store(true, std::memory_order_relaxed);
    }

    ~AllocationCount()
    {
        countingAllocations.store(false, std::memory_order_relaxed);
    }

    std::size_t bytes() const
    {
        return allocatedBytes.load(std::memory_order_relaxed);
    }
};
