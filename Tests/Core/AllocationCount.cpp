#include "AllocationCount.h"

#include <cstdlib>
#include <new>

std::atomic<bool> countingAllocations {false};
std::atomic<std::size_t> allocatedBytes {0};

void* operator new(std::size_t size)
{
    if (countingAllocations.load(std::memory_order_relaxed))
        allocatedBytes.fetch_add(size, std::memory_order_relaxed);

    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;

    throw std::bad_alloc {};
}

void* operator new[](std::size_t size)
{
    return operator new(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory) noexcept
{
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}
