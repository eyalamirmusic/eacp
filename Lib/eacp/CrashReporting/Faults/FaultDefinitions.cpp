#include "FaultDefinitions.h"

#include <eacp/Core/Threads/Thread.h>
#include <eacp/Core/Utils/Singleton.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#include <eacp/Core/Utils/WinInclude.h>
#include <intrin.h>
#endif

// Each fault has to survive the optimiser exactly as written.
#if defined(_MSC_VER)
#pragma optimize("", off)
#endif

namespace eacp::CrashReporting::Faults
{
namespace
{
constexpr auto defaultHangDuration = std::chrono::milliseconds {12000};
std::atomic<std::chrono::milliseconds> hangDuration {defaultHangDuration};
std::atomic<std::uint64_t> spinCount {0};
} // namespace

// Outside the anonymous namespace so their names reach the report's symbols.
EACP_FAULT_NO_INLINE void sleepPastHangThreshold()
{
    std::this_thread::sleep_for(hangDuration.load());
}

EACP_FAULT_NO_INLINE void spinForever()
{
    while (true)
        spinCount.fetch_add(1, std::memory_order_relaxed);
}

namespace
{
constexpr auto smallBlockSize = std::size_t {16};
constexpr auto stackFrameBytes = std::size_t {4096};
volatile auto shouldKeepRecursing = true;
} // namespace

EACP_FAULT_NO_INLINE void dereferenceNullPointer()
{
    auto* volatile target = static_cast<int*>(nullptr);
    *target = 1;
}

EACP_FAULT_NO_INLINE int recurseForever(int depth)
{
    auto frame = std::array<volatile char, stackFrameBytes> {};
    frame[0] = static_cast<char>(depth);

    if (!shouldKeepRecursing)
        return frame[0];

    return recurseForever(depth + 1) + frame[0];
}

EACP_FAULT_NO_INLINE void overflowStack()
{
    recurseForever(0);
}

struct PureVirtualBase
{
    PureVirtualBase() { callWork(); }
    virtual ~PureVirtualBase() = default;

    EACP_FAULT_NO_INLINE void callWork() { work(); }

    virtual void work() = 0;
};

struct PureVirtualDerived final : PureVirtualBase
{
    void work() override {}
};

EACP_FAULT_NO_INLINE void callPureVirtual()
{
    auto object = std::make_unique<PureVirtualDerived>();
    object->work();
}

EACP_FAULT_NO_INLINE void throwUncaughtException()
{
    throw std::runtime_error {"the crash fault threw this on purpose"};
}

#if defined(_WIN32)
// Only a fresh heap's second HeapFree is STATUS_HEAP_CORRUPTION every time.
constexpr auto heapCorruptionDescription =
    "Frees the same memory twice; Windows ends the app immediately";

EACP_FAULT_NO_INLINE void corruptHeap()
{
    auto* heap = ::HeapCreate(0, 0, 0);
    auto* volatile block = ::HeapAlloc(heap, 0, smallBlockSize);
    ::HeapFree(heap, 0, block);
    ::HeapFree(heap, 0, block);
}
#else
constexpr auto overrunSize = std::size_t {4096};
constexpr auto overrunByte = 0xEA;
constexpr auto heapCorruptionDescription =
    "Writes past the end of an allocation, then frees it twice";

// The overrun alone may land in slack; the second free is what every
// allocator checks.
EACP_FAULT_NO_INLINE void corruptHeap()
{
    auto* volatile block = static_cast<unsigned char*>(std::malloc(smallBlockSize));
    std::memset(block, overrunByte, overrunSize);
    std::free(block);
    std::free(block);
}
#endif

#if defined(_WIN32)
constexpr auto fatalAppExitFastFailCode = 7;

EACP_FAULT_NO_INLINE void failFast()
{
    __fastfail(fatalAppExitFastFailCode);
}
#endif

// x86 traps an integer divide by zero, and MSVC checks for one on ARM64;
// arm64 hardware itself returns 0, so elsewhere the fault would not fault.
#if defined(__x86_64__) || defined(_M_X64)                                          \
    || (defined(_M_ARM64) && !defined(__clang__))
constexpr auto integerDivideTraps = true;
#else
constexpr auto integerDivideTraps = false;
#endif

EACP_FAULT_NO_INLINE int divideByZero()
{
    volatile auto numerator = 1;
    volatile auto zero = 0;
    return numerator / zero;
}

namespace
{
volatile auto divideResult = 0;

struct DeadlockedLocks
{
    std::mutex first;
    std::mutex second;
    std::atomic<int> holdingCount {0};
};

DeadlockedLocks& createDeadlockedLocks()
{
    struct Registry
    {
        std::mutex mutex;
        OwnedVector<DeadlockedLocks> locks;
    };

    auto& registry = Singleton::getImmortal<Registry>();
    auto lock = std::lock_guard {registry.mutex};
    return registry.locks.createNew();
}
} // namespace

// Each caller holds one lock and waits for the other, once both hold theirs.
EACP_FAULT_NO_INLINE void lockInOppositeOrder(std::mutex& held,
                                              std::mutex& wanted,
                                              std::atomic<int>& holdingCount)
{
    constexpr auto lockHolders = 2;
    auto heldLock = std::lock_guard {held};
    holdingCount.fetch_add(1);

    while (holdingCount.load() < lockHolders)
        std::this_thread::yield();

    auto wantedLock = std::lock_guard {wanted};
}

EACP_FAULT_NO_INLINE void deadlockWithAnotherThread()
{
    auto& locks = createDeadlockedLocks();
    startBackgroundThread(
        [&locks]
        { lockInOppositeOrder(locks.second, locks.first, locks.holdingCount); });
    lockInOppositeOrder(locks.first, locks.second, locks.holdingCount);
}

Vector<Fault> makeFaults() {auto faults = Vector<Fault> {
    {"nullptr",
     "Null pointer",
     "Writes to address zero; the app crashes immediately",
     dereferenceNullPointer},
    {"divide",
     "Divide by zero",
     "Divides an integer by zero; the app crashes immediately",
     [] { divideResult = divideByZero(); },
     false,
     integerDivideTraps},
    {"stackoverflow",
     "Stack overflow",
     "Calls itself until the thread's stack is full",
     overflowStack},
    {"abort",
     "abort()",
     "Calls std::abort, which ends the app immediately",
     [] { std::abort(); }},
    {"purevirtual",
     "Pure virtual call",
     "Calls a virtual function that has no body, from a constructor",
     callPureVirtual},
    {"terminate",
     "Uncaught exception",
     "Throws an exception nothing catches, so the app ends",
     throwUncaughtException},
    {"heapcorruption", "Heap corruption", heapCorruptionDescription, corruptHeap},
    {"hang",
     "Hang",
     "Sleeps the thread longer than the hang time limit; reported as a hang",
     sleepPastHangThreshold,
     true},
    {"spin",
     "Spin",
     "Loops forever without sleeping; reported as a hang",
     spinForever,
     true},
    {"deadlock",
     "Deadlock",
     "Two threads each wait for a lock the other holds; reported as a hang",
     deadlockWithAnotherThread,
     true},
    {"fastfail",
     "__fastfail",
     "Ends the app immediately; no crash handler inside the app runs, only "
     "Windows Error Reporting sees it",
#if defined(_WIN32)
     failFast,
     false,
     true},
#else
     [] {},
     false,
     false},
#endif
};

return faults;
} // namespace eacp::CrashReporting::Faults

void setHangDuration(std::chrono::milliseconds duration)
{
    hangDuration = duration;
}

std::chrono::milliseconds getHangDuration()
{
    return hangDuration.load();
}
} // namespace eacp::CrashReporting::Faults
