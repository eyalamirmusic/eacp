#include "Timer.h"
#include "EventLoop.h"
#include "ThreadUtils-Windows.h"

#include <cassert>
#include <chrono>

#include <winrt/Windows.Foundation.h>

namespace eacp::Threads
{

struct Timer::Native
{
    Native(const Callback& cbToUse, int intervalHz)
        : cb(cbToUse)
    {
        assertMainThread();
        assert(intervalHz > 0 && "Timer interval must be positive");

        auto queue = getDispatcherQueue();
        if (!queue)
            return;

        timer = queue.CreateTimer();
        auto periodSeconds = std::chrono::duration<double>(1.0 / intervalHz);
        timer.Interval(
            std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                periodSeconds));

        tickToken = timer.Tick([this](auto&&, auto&&) { cb(); });

        timer.Start();
    }

    ~Native()
    {
        assertMainThread();

        if (timer)
        {
            timer.Stop();
            timer.Tick(tickToken);
            timer = nullptr;
        }
    }

    Callback cb;
    System::DispatcherQueueTimer timer {nullptr};
    winrt::event_token tickToken;
};

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
    , impl(cbToUse, intervalHz)
{
}

} // namespace eacp::Threads
