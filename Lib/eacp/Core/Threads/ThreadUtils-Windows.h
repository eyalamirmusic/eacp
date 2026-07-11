#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include "ThreadUtils.h"
#include <winrt/Windows.System.h>

namespace eacp::Threads
{
namespace System = winrt::Windows::System;

void initMainThread();

// Re-seeds the pre-dispatcher isMainThread() fallback to the calling
// thread. For eacp copies inside dlopen-hosted plugins, whose static-init
// capture may have run on a loader thread rather than the host's UI thread.
void setCurrentThreadAsMainFallback();

// Releases the process-wide DispatcherQueue / Controller while we
// still hold a live COM apartment. Their WinRT smart pointers would
// otherwise be destroyed at static-destruction time, AFTER the
// apartment has been torn down, and Release on a dead apartment
// crashes with STATUS_ACCESS_VIOLATION. Call once from the main
// thread just before main returns.
void shutdownMainThread();

System::DispatcherQueue getDispatcherQueue();
System::DispatcherQueueController getDispatcherQueueController();
} // namespace eacp::Threads
