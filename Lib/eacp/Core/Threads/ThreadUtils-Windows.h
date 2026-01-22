#pragma once

#include "ThreadUtils.h"
#include <cassert>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace eacp::Threads
{
void initMainThread();
DWORD getMainThreadID();
}