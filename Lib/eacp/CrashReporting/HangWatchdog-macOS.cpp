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
    thread_act_t mainThread = MACH_PORT_NULL;
    StackBounds mainStack;
    std::uint64_t reportedHeartbeat = 0;

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

void appendFrame(SharedHangInfo& hang, std::uint64_t address)
{
    if (hang.frameCount < maxCrashFrames)
        hang.frames[hang.frameCount++] = address;
}

// The suspended thread's own frame-pointer chain, from its registers. Reads
// only memory inside its stack and allocates nothing, since the thread may be
// holding the allocator's lock.
void walkFramePointers(SharedHangInfo& hang,
                       const ThreadRegisters& registers,
                       const StackBounds& stack)
{
    hang.frameCount = 0;
    appendFrame(hang, registers.programCounter);

    if (registers.returnAddress)
        appendFrame(hang, *registers.returnAddress);

    auto framePointer = registers.framePointer;

    while (hang.frameCount < maxCrashFrames
           && isFrameRecordInStack(framePointer, stack))
    {
        const auto& record = *reinterpret_cast<const FrameRecord*>(framePointer);
        auto returnAddress = stripPointerAuthentication(record.returnAddress);

        if (returnAddress == 0)
            break;

        auto isLinkRegisterAgain = hang.frameCount == 2 && registers.returnAddress
                                   && returnAddress == *registers.returnAddress;

        if (!isLinkRegisterAgain)
            appendFrame(hang, returnAddress);

        if (record.callerFramePointer <= framePointer)
            break;

        framePointer = record.callerFramePointer;
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
bool sampleMainThread(HangWatchdog& watchdog, SharedHangInfo& hang)
{
    if (::thread_suspend(watchdog.mainThread) != KERN_SUCCESS)
        return false;

    auto registers = readThreadRegisters(watchdog.mainThread);

    if (registers)
        walkFramePointers(hang, *registers, watchdog.mainStack);

    ::thread_resume(watchdog.mainThread);
    return registers.has_value();
}

// One report per episode: the heartbeat it was silent from is remembered,
// and a beat after it re-arms.
void checkForHang(HangWatchdog& watchdog)
{
    auto& hang = watchdog.state->get().hang;
    auto duration = findHangDuration(hang, getSteadyMilliseconds());
    auto heartbeat = hang.heartbeatMilliseconds.load(std::memory_order_acquire);

    if (duration == 0 || heartbeat == watchdog.reportedHeartbeat)
        return;

    if (isDebuggerAttached() || !sampleMainThread(watchdog, hang))
        return;

    hang.durationMilliseconds = duration;
    watchdog.reportedHeartbeat = heartbeat;
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

void startHangWatchdog(SharedState& state)
{
    auto& watchdog = getHangWatchdog();

    if (watchdog.thread.isJoinable())
        return;

    watchdog.state = state;
    watchdog.mainThread = ::pthread_mach_thread_np(::pthread_self());
    watchdog.mainStack = getCurrentThreadStack();
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
