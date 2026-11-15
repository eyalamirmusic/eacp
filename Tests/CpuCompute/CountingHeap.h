#pragma once

#include <atomic>

namespace ExecutorTests
{
extern std::atomic<bool> countingHeap;
extern std::atomic<int> allocationsCounted;
extern std::atomic<int> releasesCounted;
} // namespace ExecutorTests
