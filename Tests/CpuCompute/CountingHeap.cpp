#include "CountingHeap.h"

#include <algorithm>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace ExecutorTests
{
std::atomic<bool> countingHeap {false};
std::atomic<int> allocationsCounted {0};
std::atomic<int> releasesCounted {0};
} // namespace ExecutorTests

namespace
{
void noteAllocation()
{
    if (ExecutorTests::countingHeap.load(std::memory_order_relaxed))
        ExecutorTests::allocationsCounted.fetch_add(1, std::memory_order_relaxed);
}

void noteRelease(void* pointer)
{
    if (pointer != nullptr
        && ExecutorTests::countingHeap.load(std::memory_order_relaxed))
        ExecutorTests::releasesCounted.fetch_add(1, std::memory_order_relaxed);
}

void* allocateBytes(std::size_t size)
{
    noteAllocation();
    return std::malloc(size == 0 ? 1 : size);
}

void* allocateAligned(std::size_t size, std::align_val_t alignment)
{
    noteAllocation();

    auto align = std::max((std::size_t) alignment, sizeof(void*));
    auto bytes = size == 0 ? align : size;

#if defined(_WIN32)
    return _aligned_malloc(bytes, align);
#else
    void* pointer = nullptr;
    return posix_memalign(&pointer, align, bytes) == 0 ? pointer : nullptr;
#endif
}

void releaseBytes(void* pointer) noexcept
{
    noteRelease(pointer);
    std::free(pointer);
}

void releaseAligned(void* pointer) noexcept
{
    noteRelease(pointer);

#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void* allocateOrThrow(void* pointer)
{
    if (pointer == nullptr)
        throw std::bad_alloc {};

    return pointer;
}
} // namespace

void* operator new(std::size_t size)
{
    return allocateOrThrow(allocateBytes(size));
}

void* operator new[](std::size_t size)
{
    return allocateOrThrow(allocateBytes(size));
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocateOrThrow(allocateAligned(size, alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateOrThrow(allocateAligned(size, alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return allocateBytes(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return allocateBytes(size);
}

void* operator new(std::size_t size,
                   std::align_val_t alignment,
                   const std::nothrow_t&) noexcept
{
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size,
                     std::align_val_t alignment,
                     const std::nothrow_t&) noexcept
{
    return allocateAligned(size, alignment);
}

void operator delete(void* pointer) noexcept
{
    releaseBytes(pointer);
}

void operator delete[](void* pointer) noexcept
{
    releaseBytes(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept
{
    releaseBytes(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept
{
    releaseBytes(pointer);
}

void operator delete(void* pointer, std::align_val_t) noexcept
{
    releaseAligned(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept
{
    releaseAligned(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept
{
    releaseAligned(pointer);
}

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept
{
    releaseAligned(pointer);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept
{
    releaseBytes(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept
{
    releaseBytes(pointer);
}

void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept
{
    releaseAligned(pointer);
}

void operator delete[](void* pointer,
                       std::align_val_t,
                       const std::nothrow_t&) noexcept
{
    releaseAligned(pointer);
}
