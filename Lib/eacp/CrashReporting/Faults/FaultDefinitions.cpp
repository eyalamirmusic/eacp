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
#include <float.h>
#include <intrin.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <xmmintrin.h>
#endif
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

constexpr auto heapSmashLength = std::size_t {64} * 1024;
constexpr auto heapSmashByte = 0xEB;

// Only macOS captures this from outside the app, so only there is the report
// promised to survive what it does to the heap.
#if defined(__APPLE__)
constexpr auto heapSmashIsAvailable = true;
#else
constexpr auto heapSmashIsAvailable = false;
#endif

// Tramples 64 KiB of the heap after one small block, malloc's bookkeeping
// with it, then faults at once, so the crash lands before another thread's
// allocation trips over the wreck.
EACP_FAULT_NO_INLINE void smashHeap()
{
    auto* volatile block = static_cast<unsigned char*>(std::malloc(smallBlockSize));
    std::memset(block, heapSmashByte, heapSmashLength);
    dereferenceNullPointer();
}

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

// The architecture's own permanently undefined instruction.
EACP_FAULT_NO_INLINE void executeIllegalInstruction()
{
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
    __ud2();
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_ARM64)
    __emit(0x00000000);
#elif defined(__x86_64__)
    asm volatile("ud2");
#elif defined(__aarch64__)
    asm volatile(".inst 0x00000000");
#endif
}

// x64 user mode may not halt the processor; ARM64 has no separate trap for an
// instruction above its level, which is only an undefined one there.
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
constexpr auto privilegedInstructionIsAvailable = true;

EACP_FAULT_NO_INLINE void executePrivilegedInstruction()
{
    __halt();
}
#else
constexpr auto privilegedInstructionIsAvailable = false;

EACP_FAULT_NO_INLINE void executePrivilegedInstruction() {}
#endif

namespace
{
volatile auto floatingPointResult = 0.0;
} // namespace

// Only x86 implements trapping floating-point exceptions; ARM64 cores leave
// the trap enables unimplemented, so a divide there quietly gives infinity.
#if defined(_M_X64) || defined(__x86_64__)
constexpr auto floatingPointTraps = true;

EACP_FAULT_NO_INLINE void enableFloatingPointDivideTrap()
{
#if defined(_WIN32)
    auto previous = 0u;
    _controlfp_s(&previous, ~static_cast<unsigned>(_EM_ZERODIVIDE), _MCW_EM);
#else
    _MM_SET_EXCEPTION_MASK(_MM_GET_EXCEPTION_MASK() & ~_MM_MASK_DIV_ZERO);
#endif
}
#else
constexpr auto floatingPointTraps = false;

EACP_FAULT_NO_INLINE void enableFloatingPointDivideTrap() {}
#endif

EACP_FAULT_NO_INLINE double divideFloatByZero()
{
    enableFloatingPointDivideTrap();
    volatile auto numerator = 1.0;
    volatile auto zero = 0.0;
    return numerator / zero;
}

namespace
{
// Data, so its page is readable and writable but never executable.
std::array<volatile unsigned char, smallBlockSize> nonExecutableCode {};
} // namespace

EACP_FAULT_NO_INLINE void jumpToNonExecutableMemory()
{
    using Function = void (*)();
    auto* dataAddress = const_cast<unsigned char*>(nonExecutableCode.data());
    auto* volatile function = reinterpret_cast<Function>(dataAddress);
    function();
}

#if defined(_WIN32)
EACP_FAULT_NO_INLINE void touchGuardPage()
{
    auto* page = static_cast<volatile char*>(::VirtualAlloc(
        nullptr, 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD));
    page[0] = 1;
}

// A mapped read whose backing store fails is what raises this, and nothing on
// a local disk fails on demand, so the fault raises the same record directly.
EACP_FAULT_NO_INLINE void raiseInPageError()
{
    constexpr auto readAccess = ULONG_PTR {0};
    constexpr auto deviceNotReady = ULONG_PTR {0xC00000A3};
    auto* volatile address = &nonExecutableCode;
    auto arguments = std::to_array<ULONG_PTR>(
        {readAccess, reinterpret_cast<ULONG_PTR>(address), deviceNotReady});
    ::RaiseException(EXCEPTION_IN_PAGE_ERROR,
                     EXCEPTION_NONCONTINUABLE,
                     static_cast<DWORD>(arguments.size()),
                     arguments.data());
}
#else
EACP_FAULT_NO_INLINE void touchGuardPage() {}
EACP_FAULT_NO_INLINE void raiseInPageError() {}
#endif

#if defined(__APPLE__)
constexpr auto isMacOS = true;

// A page of a mapped file past the file's end, once it is truncated to nothing.
EACP_FAULT_NO_INLINE void readPastTruncatedMapping()
{
    auto path = std::to_array("/tmp/eacp-crash-fault-bus-XXXXXX");
    auto descriptor = ::mkstemp(path.data());
    ::unlink(path.data());
    auto pageSize = static_cast<std::size_t>(::getpagesize());
    ::ftruncate(descriptor, static_cast<off_t>(pageSize));
    auto* mapping = static_cast<volatile char*>(
        ::mmap(nullptr, pageSize, PROT_READ, MAP_SHARED, descriptor, 0));
    ::ftruncate(descriptor, 0);
    divideResult = mapping[0];
}

// libsystem's own descriptor guards, which have no public header.
extern "C" int change_fdguard_np(int descriptor,
                                 const std::uint64_t* guard,
                                 unsigned int guardFlags,
                                 const std::uint64_t* newGuard,
                                 unsigned int newGuardFlags,
                                 int* descriptorFlags);

// Guards a descriptor against close, then closes it. The kernel takes a close
// guard only together with a dup one.
EACP_FAULT_NO_INLINE void closeGuardedDescriptor()
{
    constexpr auto guardClose = 1u << 0;
    constexpr auto guardDuplicate = 1u << 1;
    constexpr auto guardValue = std::uint64_t {0xEACF};
    auto descriptor = ::open("/dev/null", O_RDONLY);
    auto descriptorFlags = 0;
    change_fdguard_np(descriptor,
                      nullptr,
                      0,
                      &guardValue,
                      guardClose | guardDuplicate,
                      &descriptorFlags);
    ::close(descriptor);
}

// A system call number the kernel has no entry for.
EACP_FAULT_NO_INLINE void callMissingSystemCall()
{
    constexpr auto missingSystemCall = 0x7FFF;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    ::syscall(missingSystemCall);
#pragma clang diagnostic pop
}
#else
constexpr auto isMacOS = false;

EACP_FAULT_NO_INLINE void readPastTruncatedMapping() {}
EACP_FAULT_NO_INLINE void closeGuardedDescriptor() {}
EACP_FAULT_NO_INLINE void callMissingSystemCall() {}
#endif

#if defined(_WIN32)
constexpr auto isWindows = true;
#else
constexpr auto isWindows = false;
#endif

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
    {"heapsmash",
     "Heap smash",
     "Overwrites 64 KiB of the heap, then writes through a null pointer",
     smashHeap,
     false,
     heapSmashIsAvailable},
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
    {"illegalinstruction",
     "Illegal instruction",
     "Executes the architecture's undefined instruction",
     executeIllegalInstruction},
    {"privilegedinstruction",
     "Privileged instruction",
     "Halts the processor from user mode",
     executePrivilegedInstruction,
     false,
     privilegedInstructionIsAvailable},
    {"floatingpoint",
     "Floating-point trap",
     "Unmasks the divide-by-zero trap, then divides a double by zero",
     [] { floatingPointResult = divideFloatByZero(); },
     false,
     floatingPointTraps},
    {"executenx",
     "Execute non-executable",
     "Calls into a data page",
     jumpToNonExecutableMemory},
    {"guardpage",
     "Guard page",
     "Writes to a PAGE_GUARD page",
     touchGuardPage,
     false,
     isWindows},
    {"inpageerror",
     "In-page error",
     "Raises the in-page error a failed mapped read raises",
     raiseInPageError,
     false,
     isWindows},
    {"buserror",
     "Bus error",
     "Reads a mapped file past its end once it is truncated",
     readPastTruncatedMapping,
     false,
     isMacOS},
    {"guardedfd",
     "Guarded descriptor",
     "Closes a file descriptor guarded against close",
     closeGuardedDescriptor,
     false,
     isMacOS},
    {"sigsys",
     "Bad system call",
     "Makes a system call that does not exist",
     callMissingSystemCall,
     false,
     isMacOS},
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
