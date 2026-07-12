#include "Async.h"

#include <thread>

namespace eacp::Threads
{
Async<void> delay(Time::MS duration)
{
    auto promise = AsyncPromise<void>();
    std::thread(
        [promise, duration]
        {
            Time::sleep(duration);
            callAsync([promise] { promise.resolve(); });
        })
        .detach();
    return promise.get();
}
} // namespace eacp::Threads
