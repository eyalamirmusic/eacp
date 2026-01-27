#pragma once

#include "ThreadUtils.h"
#include <cassert>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DispatcherQueue.h>

#include <winrt/Windows.System.h>

namespace eacp::Threads
{
void initMainThread();
DWORD getMainThreadID();
winrt::Windows::System::DispatcherQueue getDispatcherQueue();
}
