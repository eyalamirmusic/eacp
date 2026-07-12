#include "Async.h"

#include <thread>

namespace eacp::Threads
{
Async<void> delay(std::chrono::milliseconds duration)
{
    auto promise = AsyncPromise<void>();
    std::thread(
        [promise, duration]
        {
            std::this_thread::sleep_for(duration);
            callAsync([promise] { promise.resolve(); });
        })
        .detach();
    return promise.get();
}
} // namespace eacp::Threads
