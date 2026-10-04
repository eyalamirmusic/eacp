#pragma once

#include <cstdint>

namespace eacp::Threads
{
void assertMainThread();
bool isMainThread();

std::uint64_t currentThreadId();

} // namespace eacp::Threads
