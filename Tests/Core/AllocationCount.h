#pragma once

#include <atomic>
#include <cstddef>

// What an operation costs in allocations has no other observable: the bytes are
// the same however they were assembled, and unlike timing the count does not
// depend on the machine or the build. The global operator new that feeds these
// lives in AllocationCount.cpp, so the test binary replaces it exactly once.
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
