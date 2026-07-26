#include "../Utils/WinInclude.h"

#include "Timer.h"
#include "EventLoop.h"
#include "ThreadUtils-Windows.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

namespace eacp::Threads
{
namespace
{
// Shared between the ticking thread, ticks already queued on the main thread
// and the timer itself, so a tick still in the event queue when the timer is
// stopped fizzles instead of touching a dead callback.
struct TickState
{
    explicit TickState(const Callback& cbToUse)
        : cb(cbToUse)
    {
    }

    Callback cb;
    std::atomic<bool> alive {true};
    std::atomic<bool> pending {false};
};

double nowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

// A dedicated thread waits on a high-resolution timer and posts each tick to
// the main thread — the same shape as DisplayLink-Windows. This replaces
// SetTimer/WM_TIMER, whose ticks quantise to the ~15.6 ms scheduler tick and
// are delivered at the queue's lowest priority: a "100 Hz" timer really ran at
// ~64 Hz and starved outright while the queue was busy. The posted wake rides
// the message-only window callAsync uses, so ticks keep flowing inside the OS
// modal resize/move loop just as WM_TIMER's did. Ticks step an absolute
// schedule (no cumulative drift) and coalesce: while one is still queued
// behind a busy main thread, further ones are skipped rather than piling up.
// Callers wanting frame-accurate ticks should use DisplayLink, as before.
struct Timer::Native
{
    Native(const Callback& cbToUse, int intervalHz)
        : state(std::make_shared<TickState>(cbToUse))
    {
        assertMainThread();
        assert(intervalHz > 0 && "Timer interval must be positive");

        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread =
            std::thread([this, period = 1.0 / intervalHz] { tickLoop(period); });
    }

    ~Native() { stop(); }

    // Safe from inside the timer's own callback: the callback runs on the
    // main thread, and the thread being joined only ever posts.
    void stop()
    {
        assertMainThread();

        if (!thread.joinable())
            return;

        state->alive = false;
        SetEvent(stopEvent);
        thread.join();
        CloseHandle(stopEvent);
        stopEvent = nullptr;
    }

    Native(const Native&) = delete;
    Native& operator=(const Native&) = delete;

private:
    void tickLoop(double period)
    {
        // The high-resolution timer holds sub-millisecond cadence; the plain
        // one is the pre-1803 fallback.
        auto timer = CreateWaitableTimerExW(nullptr,
                                            nullptr,
                                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);

        if (timer == nullptr)
            timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

        auto next = nowSeconds() + period;

        while (state->alive)
        {
            auto remaining = next - nowSeconds();

            if (remaining <= 0.0)
            {
                postTick();
                next += period;

                // A stall longer than a period restarts the cadence instead
                // of bursting to catch up.
                if (next <= nowSeconds())
                    next = nowSeconds() + period;

                continue;
            }

            if (!waitFor(timer, remaining))
                break;
        }

        if (timer != nullptr)
            CloseHandle(timer);
    }

    // Blocks until the wait elapses, returning false once the stop event is
    // signalled so the loop ends.
    bool waitFor(HANDLE timer, double seconds) const
    {
        if (timer != nullptr)
        {
            auto due = LARGE_INTEGER {};
            due.QuadPart = -(LONGLONG) (seconds * 1e7);

            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            {
                HANDLE handles[2] = {stopEvent, timer};
                return WaitForMultipleObjects(2, handles, FALSE, INFINITE)
                       != WAIT_OBJECT_0;
            }
        }

        auto ms = (DWORD) (seconds * 1000.0) + 1;
        return WaitForSingleObject(stopEvent, ms) == WAIT_TIMEOUT;
    }

    void postTick() const
    {
        if (state->pending.exchange(true))
            return;

        callAsync(
            [state = state]
            {
                state->pending = false;

                if (state->alive)
                    state->cb();
            });
    }

    std::shared_ptr<TickState> state;
    HANDLE stopEvent = nullptr;
    std::thread thread;
};

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
    , impl(cbToUse, intervalHz)
{
}

void Timer::stop()
{
    impl->stop();
}

} // namespace eacp::Threads
