#pragma once

#include <cstddef>
#include <string_view>

// The descriptors the handler process is started with: posix_spawn moves the
// app's ends onto these numbers and closes everything else.
namespace eacp::CrashReporting::Detail
{
constexpr auto handlerSharedStateDescriptor = 3;
constexpr auto handlerCrashPipeDescriptor = 4;
constexpr auto handlerReadyPipeDescriptor = 5;
constexpr auto firstFreeDescriptor = handlerReadyPipeDescriptor + 1;

// The <channel> word on the handler's command line, after --crash-handler and
// the database (Windows puts its named objects' prefix there). On macOS the
// connection is always descriptors 3-5 above, so the word only says which of
// two starts this is, and a command line with any other word is refused.

// Pipes and shared block only: the app crashes through the in-app signal
// handler (Options::captureMachExceptions off).
constexpr auto signalOnlyHandlerChannel = std::string_view {"inherited-descriptors"};

// The same, and the handler's bootstrap port is a Mach port of the app's, over
// which it asks for the app's task port (TaskPortHandshake-macOS.h).
constexpr auto machHandshakeHandlerChannel =
    std::string_view {"inherited-descriptors-mach-handshake"};

constexpr auto crashNotificationByte = char {'C'};
constexpr auto hangNotificationByte = char {'H'};
constexpr auto handlerReadyByte = char {'R'};
} // namespace eacp::CrashReporting::Detail
