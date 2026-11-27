#pragma once

#include "GlobalMouseMonitor.h"

#include <eacp/Core/Threads/EventLoop.h>

namespace eacp::Graphics
{
// Runs the handler with `event` on the main thread, later: a handler that
// destroys its own monitor, as dismissing an overlay does, does it after the
// platform's callback has returned. A monitor destroyed in between drops it.
inline void
    deliverOnMainThread(const std::shared_ptr<GlobalMouseMonitor::Handler>& handler,
                        const MouseEvent& event)
{
    Threads::callAsync(
        [weakHandler = std::weak_ptr<GlobalMouseMonitor::Handler>(handler), event]
        {
            if (auto liveHandler = weakHandler.lock())
                (*liveHandler)(event);
        });
}
} // namespace eacp::Graphics
