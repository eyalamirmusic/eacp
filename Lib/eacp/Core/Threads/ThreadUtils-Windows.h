#pragma once

#include "ThreadUtils.h"
#include <winrt/Windows.System.h>

namespace eacp::Threads
{
namespace System = winrt::Windows::System;

void initMainThread();

System::DispatcherQueue getDispatcherQueue();
System::DispatcherQueueController getDispatcherQueueController();
}
