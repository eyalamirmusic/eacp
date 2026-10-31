#pragma once

#include <cstddef>
#include <string_view>

namespace eacp::CrashReporting::Detail
{
// The fixed file descriptor numbers at which the app hands the handler process
// its shared memory and pipes, after stdin, stdout and stderr (0, 1 and 2).
constexpr auto handlerSharedMemoryFileDescriptor = 3;
constexpr auto handlerCrashPipeFileDescriptor = 4;
constexpr auto handlerReadyPipeFileDescriptor = 5;
constexpr auto firstFileDescriptorAfterHandlerSlots =
    handlerReadyPipeFileDescriptor + 1;

// One of these is an argument on the handler's command line, telling it which
// of two connection setups the app started it with; any other value is refused.

// The pipes and shared memory only (Options::captureMachExceptions off).
constexpr auto signalOnlyConnectionArgument =
    std::string_view {"inherited-descriptors"};

// The same pipes and shared memory, plus a Mach port the handler uses to ask
// the app for its task port, which lets it read the app's memory from outside.
constexpr auto machTaskPortConnectionArgument =
    std::string_view {"inherited-descriptors-mach-handshake"};

constexpr auto crashNotificationByte = char {'C'};
constexpr auto hangNotificationByte = char {'H'};
constexpr auto handlerReadyByte = char {'R'};
} // namespace eacp::CrashReporting::Detail
