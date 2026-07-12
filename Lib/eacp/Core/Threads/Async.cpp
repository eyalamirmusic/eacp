#include "Async.h"
#include "../Utils/StdChrono.h"

#include <thread>

namespace eacp::Threads
{
Async<void> delay(Time::MS duration)
{
    auto promise = AsyncPromise<void>();
    std::thread(
        [promise, duration]
        {
            std::this_thread::sleep_for(toStdChrono(duration));
            callAsync([promise] { promise.resolve(); });
        })
        .detach();
    return promise.get();
}
} // namespace eacp::Threads
