#include "Async.h"

namespace eacp::Threads
{
std::string detail::messageOf(const std::exception_ptr& error)
{
    try
    {
        std::rethrow_exception(error);
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
    catch (...)
    {
        return "Unknown exception in Async coroutine";
    }
}

Async<void> delay(Time::MS duration)
{
    auto promise = AsyncPromise<void>();
    callAfter(duration, [promise] { promise.resolve(); });
    return promise.get();
}
} // namespace eacp::Threads
