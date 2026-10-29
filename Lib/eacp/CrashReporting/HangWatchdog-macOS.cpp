#include "CrashClient-macOS.h"
#include "CrashReportingPlatform.h"
#include "MachImages-macOS.h"

#include <eacp/Core/Threads/Thread.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <mach/mach.h>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace eacp::CrashReporting::Detail
{
namespace
{
struct StackBounds
{
    std::uint64_t low = 0;
    std::uint64_t high = 0;
};

struct ThreadRegisters
{
    std::uint64_t programCounter = 0;
    std::uint64_t framePointer = 0;

    // arm64's link register; x86_64 keeps the return address on the stack.
    std::optional<std::uint64_t> returnAddress;
};

struct HangWatchdog
{
    std::optional<std::reference_wrapper<SharedState>> state;
    StallTracker tracker;

    std::mutex mutex;
    std::condition_variable wake;
    bool stopRequested = false;
    Threads::Thread thread;
};

// Never destroyed: its thread joins on destruction, and an app that never
// calls stop() would otherwise hang in exit.
HangWatchdog& getHangWatchdog()
{
    static auto* watchdog = new HangWatchdog {};
    return *watchdog;
}
// P_TRACED, which a debugger sets on attaching and lldb's pause leaves set.
bool isDebuggerAttached()
{
    auto query = std::array<int, 4> {CTL_KERN, KERN_PROC, KERN_PROC_PID, ::getpid()};
    auto info = kinfo_proc {};
    auto size = sizeof(info);

    if (::sysctl(query.data(), query.size(), &info, &size, nullptr, 0) != 0)
        return false;

    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

#if defined(__arm64__)
std::optional<ThreadRegisters> readThreadRegisters(thread_act_t thread)
{
    auto registers = arm_thread_state64_t {};
    auto count = mach_msg_type_number_t {ARM_THREAD_STATE64_COUNT};
    auto* raw = reinterpret_cast<thread_state_t>(&registers);

    if (::thread_get_state(thread, ARM_THREAD_STATE64, raw, &count) != KERN_SUCCESS)
        return std::nullopt;

    auto programCounter = arm_thread_state64_get_pc(registers);
    auto framePointer = arm_thread_state64_get_fp(registers);
    auto linkRegister = arm_thread_state64_get_lr(registers);

    return ThreadRegisters {stripPointerAuthentication(programCounter),
                            stripPointerAuthentication(framePointer),
                            stripPointerAuthentication(linkRegister)};
}
#else
std::optional<ThreadRegisters> readThreadRegisters(thread_act_t thread)
{
    auto registers = x86_thread_state64_t {};
    auto count = mach_msg_type_number_t {x86_THREAD_STATE64_COUNT};
    auto* raw = reinterpret_cast<thread_state_t>(&registers);

    if (::thread_get_state(thread, x86_THREAD_STATE64, raw, &count) != KERN_SUCCESS)
        return std::nullopt;

    return ThreadRegisters {registers.__rip, registers.__rbp, std::nullopt};
}
#endif

// A frame record is the caller's frame pointer and then the return address,
// both inside the stack and pointer-aligned.
bool isFrameRecordInStack(std::uint64_t framePointer, const StackBounds& stack)
{
    constexpr auto frameRecordBytes = 2 * sizeof(std::uint64_t);
    constexpr auto frameAlignment = sizeof(std::uint64_t);

    return framePointer % frameAlignment == 0 && framePointer >= stack.low
           && framePointer + frameRecordBytes <= stack.high;
}

struct FrameRecord
{
    std::uint64_t callerFramePointer;
    std::uint64_t returnAddress;
};

void appendFrame(SharedStalledThread& stalled, std::uint64_t address)
{
    if (stalled.frameCount < maxCrashFrames)
        stalled.frames[stalled.frameCount++] = address;
}

// Reads a frame record in this process, which the walk has already checked
// lies inside the stack.
struct InProcessFrameReader
{
    std::optional<FrameRecord> operator()(std::uint64_t framePointer) const
    {
        return *reinterpret_cast<const FrameRecord*>(framePointer);
    }
};

// The suspended thread's own frame-pointer chain, from its registers. Reads
// only memory inside its stack, through `readRecord` (in this process or
// another's), and allocates nothing, since the thread may be holding the
// allocator's lock.
template <typename FrameRecordReader>
void walkFramePointers(SharedStalledThread& stalled,
                       const ThreadRegisters& registers,
                       const StackBounds& stack,
                       const FrameRecordReader& readRecord)
{
    stalled.frameCount = 0;
    appendFrame(stalled, registers.programCounter);

    if (registers.returnAddress)
        appendFrame(stalled, *registers.returnAddress);

    auto framePointer = registers.framePointer;

    while (stalled.frameCount < maxCrashFrames
           && isFrameRecordInStack(framePointer, stack))
    {
        auto record = readRecord(framePointer);

        if (!record)
            break;

        auto returnAddress = stripPointerAuthentication(record->returnAddress);

        if (returnAddress == 0)
            break;

        auto isLinkRegisterAgain = stalled.frameCount == 2 && registers.returnAddress
                                   && returnAddress == *registers.returnAddress;

        if (!isLinkRegisterAgain)
            appendFrame(stalled, returnAddress);

        if (record->callerFramePointer <= framePointer)
            break;

        framePointer = record->callerFramePointer;
    }
}

// pthread's stack address is the top: the stack grows down from it.
StackBounds getCurrentThreadStack()
{
    auto* self = ::pthread_self();
    auto high = reinterpret_cast<std::uintptr_t>(::pthread_get_stackaddr_np(self));
    auto size = static_cast<std::uint64_t>(::pthread_get_stacksize_np(self));
    return {high - size, high};
}
bool sampleWatchedThread(const SharedWatchedThread& thread,
                         SharedStalledThread& stalled)
{
    auto port = static_cast<thread_act_t>(thread.samplingHandle);
    auto stack = StackBounds {thread.stackLow, thread.stackHigh};

    if (port == MACH_PORT_NULL || port == ::pthread_mach_thread_np(::pthread_self())
        || ::thread_suspend(port) != KERN_SUCCESS)
        return false;

    auto registers = readThreadRegisters(port);

    if (registers)
        walkFramePointers(stalled, *registers, stack, InProcessFrameReader {});

    ::thread_resume(port);
    return registers.has_value();
}

// Every stalled slot sampled into hang.stalled, then one notification: a
// deadlock between watched threads is one report naming each of them.
void checkForHang(HangWatchdog& watchdog)
{
    auto& hang = watchdog.state->get().hang;
    auto stalls = takeNewStalls(hang, watchdog.tracker, getSteadyMilliseconds());

    if (stalls.empty() || isDebuggerAttached())
        return;

    auto count = std::uint32_t {0};

    for (const auto& stall: stalls)
    {
        const auto& thread = hang.threads[stall.slot];
        auto& stalled = hang.stalled[count];

        if (!sampleWatchedThread(thread, stalled)
            || !isSameWatchedThread(thread, stall))
            continue;

        stalled.threadId = thread.threadId;
        stalled.name = thread.name;
        stalled.durationMilliseconds = stall.durationMilliseconds;
        ++count;
    }

    if (count == 0)
        return;

    hang.stalledCount = count;
    notifyHandlerOfHang();
}

bool waitForStopRequest(HangWatchdog& watchdog, std::chrono::milliseconds timeout)
{
    auto lock = std::unique_lock {watchdog.mutex};
    return watchdog.wake.wait_for(
        lock, timeout, [&watchdog] { return watchdog.stopRequested; });
}

void runHangWatchdog(HangWatchdog& watchdog)
{
    auto interval = std::chrono::milliseconds {
        watchdog.state->get().hang.pollIntervalMilliseconds};

    while (!waitForStopRequest(watchdog, interval))
        checkForHang(watchdog);
}
} // namespace

void recordThreadForSampling(SharedWatchedThread& thread)
{
    auto stack = getCurrentThreadStack();
    thread.samplingHandle = ::pthread_mach_thread_np(::pthread_self());
    thread.stackLow = stack.low;
    thread.stackHigh = stack.high;
}

void startHangWatchdog(SharedState& state)
{
    auto& watchdog = getHangWatchdog();

    if (watchdog.thread.isJoinable())
        return;

    watchdog.state = state;
    watchdog.tracker = {};
    watchdog.stopRequested = false;
    watchdog.thread.start([&watchdog] { runHangWatchdog(watchdog); });
}

void stopHangWatchdog()
{
    auto& watchdog = getHangWatchdog();

    if (!watchdog.thread.isJoinable())
        return;

    {
        auto lock = std::lock_guard {watchdog.mutex};
        watchdog.stopRequested = true;
    }

    watchdog.wake.notify_all();
    watchdog.thread.join();
}
} // namespace eacp::CrashReporting::Detail
