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

// Reading a crashed app from the handler process through its task port: the
// faulting thread's registers, its memory, its stack and its images.
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

struct FaultingThreadRegisters
{
    NativeThreadState state {};
    std::uint64_t programCounter = 0;
    std::uint64_t stackPointer = 0;
    std::uint64_t framePointer = 0;
    std::uint64_t linkRegister = 0;
};

// The thread's registers as it faulted; the copy the exception message
// carried when the thread itself cannot be asked.
std::optional<FaultingThreadRegisters>
    readThreadState(const MachException& exception);

bool readAppMemory(mach_port_t task,
                   std::uint64_t address,
                   std::span<std::byte> destination);

// The faulting pc, then each return address up the frame-pointer chain.
Vector<std::uint64_t> walkFramePointers(mach_port_t task,
                                        const FaultingThreadRegisters& registers,
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
