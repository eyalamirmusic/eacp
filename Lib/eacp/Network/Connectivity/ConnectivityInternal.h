#pragma once

#include "Connectivity.h"

namespace eacp::Network::Connectivity::detail
{
// Brings the platform monitor up. Called once per process, from the first
// Monitor::get(). It must seed the state with a cheap synchronous read
// before it returns - the caller reads the state the moment it comes back -
// and then keep reporting.
//
// It must not call Monitor::get(): the start is guarded by a once-flag that
// re-entering would deadlock. Report through reportState instead, which is
// free to be called from inside it.
void startPlatformMonitoring();

// What the platform just saw, from whatever thread the OS called on. Lands
// on the message thread, where it becomes Monitor::setState.
void reportState(const State& state);
} // namespace eacp::Network::Connectivity::detail
