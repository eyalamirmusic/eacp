#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include "ThreadUtils.h"
#include <winrt/Windows.System.h>

namespace eacp::Threads
{
namespace System = winrt::Windows::System;

void initMainThread();

// Releases the process-wide DispatcherQueue / Controller while we
// still hold a live COM apartment. Their WinRT smart pointers would
// otherwise be destroyed at static-destruction time, AFTER the
// apartment has been torn down, and Release on a dead apartment
// crashes with STATUS_ACCESS_VIOLATION. Call once from the main
// thread just before main returns.
void shutdownMainThread();

System::DispatcherQueue getDispatcherQueue();
System::DispatcherQueueController getDispatcherQueueController();
}
