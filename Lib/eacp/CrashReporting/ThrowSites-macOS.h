#pragma once

#include "SharedState.h"

// The stack of every C++ throw in this image, kept per thread, so an uncaught
// exception is reported where it was thrown rather than where std::terminate
// ran. This image's own __cxa_throw takes the record and calls the real one;
// throws from inside another dynamic library go straight to libc++abi and
// are not recorded.
namespace eacp::CrashReporting::Detail
{
void startRecordingThrowSites();
void stopRecordingThrowSites();

// Copies the calling thread's last throw into crash.throwFrames when it is
// the exception in flight. False, and nothing copied, otherwise.
bool copyThrowSiteOfCurrentException(SharedCrashInfo& crash);
} // namespace eacp::CrashReporting::Detail
