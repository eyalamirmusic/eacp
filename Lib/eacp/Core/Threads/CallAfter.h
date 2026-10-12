#pragma once

namespace eacp::Threads
{
// Joins callAfter's scheduler thread and drops every callback it still held;
// the next callAfter starts a new one. The half of detachCurrentThreadAsMain
// every platform shares: a hosted copy about to be unloaded cannot leave a
// thread running whose code lives in the image. Internal to eacp.
void stopCallAfterScheduler();
} // namespace eacp::Threads
