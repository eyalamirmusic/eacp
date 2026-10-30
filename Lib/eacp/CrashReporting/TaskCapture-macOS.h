#pragma once

#include "MachExceptionMessage-macOS.h"

#include <eacp/Core/Utils/Containers.h>

#include <mach/mach.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// Run in the handler process after the app has crashed. Through the app's task
// port (the handle that lets one macOS process read another) they read the
// crashed thread's registers, the app's memory, its stack and loaded libraries.
namespace eacp::CrashReporting::Detail
{
#if defined(__arm64__)
using NativeThreadState = arm_thread_state64_t;
constexpr auto nativeThreadStateFlavor = thread_state_flavor_t {ARM_THREAD_STATE64};
constexpr auto nativeThreadStateCount =
    mach_msg_type_number_t {ARM_THREAD_STATE64_COUNT};
#else
using NativeThreadState = x86_thread_state64_t;
constexpr auto nativeThreadStateFlavor = thread_state_flavor_t {x86_THREAD_STATE64};
constexpr auto nativeThreadStateCount =
    mach_msg_type_number_t {x86_THREAD_STATE64_COUNT};
#endif

struct TaskThreadRegisters
{
    NativeThreadState state {};
    std::uint64_t programCounter = 0;
    std::uint64_t stackPointer = 0;
    std::uint64_t framePointer = 0;
    std::uint64_t linkRegister = 0;
};

// Any thread of the task, as it is now.
std::optional<TaskThreadRegisters> readTaskThreadRegisters(mach_port_t thread);

// Every thread of the task; the caller releases each with
// releaseTaskThreads.
Vector<mach_port_t> listTaskThreads(mach_port_t task);
void releaseTaskThreads(const Vector<mach_port_t>& threads);

// The thread's registers as it faulted; the copy the exception message
// carried when the thread itself cannot be asked.
std::optional<TaskThreadRegisters> readThreadState(const MachException& exception);

bool readAppMemory(mach_port_t task,
                   std::uint64_t address,
                   std::span<std::byte> destination);

// The faulting pc, then each return address up the frame-pointer chain.
Vector<std::uint64_t> walkFramePointers(mach_port_t task,
                                        const TaskThreadRegisters& registers,
                                        std::size_t maxFrames);

struct AppImage
{
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::string path;
    std::array<std::uint8_t, 16> uuid {};
};

// Every image dyld has loaded into the task, dyld included.
Vector<AppImage> readImageList(mach_port_t task);

// The OS's id for the thread, as pthread_threadid_np gives it in the app.
std::uint64_t readThreadId(mach_port_t thread);

struct MemoryRange
{
    std::uint64_t start = 0;
    std::uint64_t end = 0;
};

// The thread's stack from its stack pointer to the top of the mapping it is
// in, at most maxBytes.
std::optional<MemoryRange> findThreadStack(mach_port_t task,
                                           std::uint64_t stackPointer,
                                           std::size_t maxBytes);
} // namespace eacp::CrashReporting::Detail
