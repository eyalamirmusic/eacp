#pragma once

namespace eacp::Threads
{
// Joins callAfter's scheduler thread and drops every callback it still held;
// the next callAfter starts a new one. Called by Apps::run after the shared
// pool stops, and by detachCurrentThreadAsMain on every platform: a hosted copy
// about to be unloaded cannot leave a thread running whose code lives in the
// image. Internal to eacp.
void stopCallAfterScheduler();
} // namespace eacp::Threads
