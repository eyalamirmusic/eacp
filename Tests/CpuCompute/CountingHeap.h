#pragma once

#include <atomic>

namespace ExecutorTests
{
extern std::atomic<bool> countingHeap;
extern std::atomic<int> allocationsCounted;
extern std::atomic<int> releasesCounted;

// False under ThreadSanitizer, whose own operator new and delete replace the
// counting ones, so nothing is counted.
#if defined(EACP_THREAD_SANITIZER)
inline constexpr bool isCountingHeapLinked = false;
#else
inline constexpr bool isCountingHeapLinked = true;
#endif
} // namespace ExecutorTests
