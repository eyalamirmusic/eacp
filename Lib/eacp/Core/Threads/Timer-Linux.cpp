#include "Timer.h"
#include "EventLoop.h"
#include "ThreadUtils.h"
#include <chrono>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace eacp::Threads
{

struct Timer::Native
{
    // Ticks are posted from the worker to the loop, so one can already be
    // queued when the timer dies. The main thread clears `alive` in the
    // destructor and the queued tick, which runs on the main thread after it,
    // finds it cleared.
    struct State
    {
        Callback cb;
        bool alive = true;
    };

    Native(const Callback& cbToUse, double intervalSec)
        : state(std::make_shared<State>(cbToUse, true))
        , period(std::chrono::duration<double>(intervalSec))
    {
        assertMainThread();
        assert(intervalSec > 0 && "Timer interval must be positive");

        running = true;
        worker = std::thread([this] { tick(); });
    }

    ~Native()
    {
        assertMainThread();
        state->alive = false;
        {
            auto lock = std::lock_guard(mutex);
            running = false;
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }

    void tick()
    {
        while (true)
        {
            auto lock = std::unique_lock(mutex);
            cv.wait_for(lock, period, [this] { return !running; });
            if (!running)
                return;
            lock.unlock();
            callAsync(
                [held = state]
                {
                    if (held->alive)
                        held->cb();
                });
        }
    }

    std::shared_ptr<State> state;
    std::chrono::duration<double> period;
    bool running = false;
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
};

Timer::Timer(const Callback& cbToUse, Time::MS interval)
    : callback(cbToUse)
    , impl(cbToUse, (double) interval.count / 1000.0)
{
}

Timer::Timer(const Callback& cbToUse, int intervalHz)
    : callback(cbToUse)
    , impl(cbToUse, 1.0 / (double) intervalHz)
{
}

} // namespace eacp::Threads
