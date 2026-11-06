#pragma once

/** @file Main-thread checks and the current thread's id. */

#include <cstdint>

namespace eacp::Threads
{
/// Asserts in debug builds when called off the main thread.
void assertMainThread();
bool isMainThread();

std::uint64_t currentThreadId();

} // namespace eacp::Threads
