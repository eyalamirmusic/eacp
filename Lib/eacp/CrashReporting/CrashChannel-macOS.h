#pragma once

#include <cstddef>

// The descriptors the handler process is started with: posix_spawn moves the
// app's ends onto these numbers and closes everything else.
namespace eacp::CrashReporting::Detail
{
constexpr auto handlerSharedStateDescriptor = 3;
constexpr auto handlerCrashPipeDescriptor = 4;
constexpr auto handlerReadyPipeDescriptor = 5;
constexpr auto firstFreeDescriptor = handlerReadyPipeDescriptor + 1;

// The handler's channel argument; it names nothing, since the descriptors are
// fixed, and is checked so a stray command line is refused.
constexpr auto handlerDescriptorChannel = "inherited-descriptors";

constexpr auto crashNotificationByte = char {'C'};
constexpr auto hangNotificationByte = char {'H'};
constexpr auto handlerReadyByte = char {'R'};
} // namespace eacp::CrashReporting::Detail
