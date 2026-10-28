#pragma once

#include <mach/mach.h>

#include <array>
#include <cstdint>
#include <optional>

// The handler's side of the exception port: the kernel's message for a crash
// exception in the app, as EXCEPTION_STATE_IDENTITY with 64-bit codes sends
// it, and the reply that releases the faulting thread.
namespace eacp::CrashReporting::Detail
{
// One exception, copied out of the kernel's message: which exception and its
// two codes, send rights to the faulting thread and its task, the thread's
// registers as it faulted, and where the reply goes. The message itself is
// laid out in the .cpp.
struct MachException
{
    exception_type_t type = 0;
    std::array<std::int64_t, 2> codes {};
    mach_port_t thread = MACH_PORT_NULL;
    mach_port_t task = MACH_PORT_NULL;
    thread_state_flavor_t stateFlavor = 0;
    mach_msg_type_number_t stateCount = 0;
    std::array<natural_t, THREAD_STATE_MAX> state {};
    mach_port_t replyPort = MACH_PORT_NULL;
    mach_msg_id_t requestId = 0;
};

// Blocks for the next exception. Empty once the port is destroyed, or for a
// message that is not an exception, which is dropped.
std::optional<MachException> receiveExceptionMessage(mach_port_t exceptionPort);

// KERN_FAILURE lets the kernel go on to the next handler and the default
// action, the signal; KERN_SUCCESS says the exception was dealt with.
void replyToException(const MachException& exception, kern_return_t result);

// Drops the send rights the message carried for the thread and the task.
void releaseExceptionPorts(const MachException& exception);

// The reason a report names it by: EXC_BAD_ACCESS and the rest.
const char* getMachExceptionName(exception_type_t type);
} // namespace eacp::CrashReporting::Detail
