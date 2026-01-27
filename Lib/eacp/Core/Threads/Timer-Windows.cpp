#include "Timer.h"
#include "ThreadUtils.h"
#include "EventLoop.h"
#include "ThreadUtils-Windows.h"

#include <winrt/Windows.Foundation.h>

namespace eacp::Threads
{

struct Timer::Native
{
    Native(const Callback& cbToUse, int intervalHz)
        : cb(cbToUse)
    {
        assertMainThread();

        auto queue = getDispatcherQueue();
        if (!queue)
        {
            return;
        }

        timer = queue.CreateTimer();
        auto intervalMs = std::chrono::milliseconds(1000 / intervalHz);
        timer.Interval(intervalMs);

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
    winrt::Windows::System::DispatcherQueueTimer timer {nullptr};
    winrt::event_token tickToken;
};

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
    , impl(cbToUse, intervalHz)
{
}

} // namespace eacp::Threads
