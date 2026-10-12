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

// Kills every thread timer still registered (Timer-Windows.cpp), so WM_TIMER
// never again dispatches to a procedure in an image about to unload. The owning
// Timers, singleton-held ones as a rule since anything else is destroyed by
// then, keep a dead id their destructor skips. For detachCurrentThreadAsMain.
void stopLiveTimers();

} // namespace eacp::Threads
