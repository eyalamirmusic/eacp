#pragma once

#include "../Utils/WinInclude.h"

#include "ThreadUtils.h"

namespace eacp::Threads
{

// Records the calling thread as the main thread. Previously this also stood up a
// WinRT DispatcherQueue, which existed for exactly one reason: Windows.UI
// .Composition's Compositor refuses to be created on a thread without one. The
// compositor is DirectComposition now, which has no such requirement, so the
// dispatcher — and the static-destruction crash it caused (releasing WinRT smart
// pointers after the COM apartment had already been torn down) — is gone.
void initMainThread();

// Re-seeds the main-thread identity to the calling thread. For eacp copies inside
// dlopen-hosted plugins, whose static-init capture may have run on a loader
// thread rather than the host's UI thread.
void setCurrentThreadAsMainFallback();

void shutdownMainThread();

} // namespace eacp::Threads
