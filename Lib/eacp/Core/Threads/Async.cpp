#include "Async.h"

namespace eacp::Threads
{
Async<void> delay(Time::MS duration)
{
    auto promise = AsyncPromise<void>();
    callAfter(duration, [promise] { promise.resolve(); });
    return promise.get();
}
} // namespace eacp::Threads
