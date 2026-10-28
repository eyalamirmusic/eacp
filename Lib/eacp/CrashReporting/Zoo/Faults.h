#pragma once

#include "Zoo.h"

#include <eacp/Core/Utils/Common.h>

#if defined(_MSC_VER)
#define EACP_ZOO_NO_INLINE __declspec(noinline)
#else
#define EACP_ZOO_NO_INLINE [[gnu::noinline]]
#endif

namespace eacp::CrashReporting::Zoo
{
Vector<Fault> makeFaults();

// A thread of its own, never joined: it may crash, stall or never return.
void startBackgroundThread(const Callback& body);
} // namespace eacp::CrashReporting::Zoo
