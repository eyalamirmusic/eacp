#pragma once

#include "../Utils/WinInclude.h"

#include "ThreadUtils.h"

namespace eacp::Threads
{

void initMainThread();

// Re-seeds the main-thread identity to the calling thread, for eacp copies in
// dlopen-hosted plugins whose static-init capture ran on a loader thread.
void setCurrentThreadAsMainFallback();

void shutdownMainThread();

} // namespace eacp::Threads
