// Windows implementation of DisplayLink
// Uses a high-frequency timer to simulate V-sync callbacks
#include "DisplayLink.h"

#include <eacp/Core/Threads/Timer.h>

namespace eacp::Threads
{

struct DisplayLink::Native
{
    Native(const Callback& cb)
        : timer(cb, 60) // 60 Hz refresh rate
    {
    }

    Timer timer;
};

DisplayLink::DisplayLink(const Callback& cbToUse)
    : callback(cbToUse)
    , impl(cbToUse)
{
}

} // namespace eacp::Threads
