#include "Timer.h"
#include "EventLoop.h"
#include "ThreadUtils.h"
#include <chrono>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace eacp::Threads
{

struct Timer::Native
{
    Native(const Callback& cbToUse, int intervalHz)
        : cb(cbToUse)
        , period(std::chrono::duration<double>(1.0 / intervalHz))
    {
        assertMainThread();
        assert(intervalHz > 0 && "Timer interval must be positive");

        running = true;
        worker = std::thread([this] { tick(); });
    }

    ~Native() { stop(); }

    // The callback runs on the main thread via callAsync, never on the
    // worker, so joining here cannot deadlock — including when stop() is
    // called from inside the callback itself.
    void stop()
    {
        assertMainThread();
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
            callAsync(cb);
        }
    }

    Callback cb;
    std::chrono::duration<double> period;
    bool running = false;
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
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
