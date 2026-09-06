#include "EventLoop.h"
#include "../Utils/Singleton.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace eacp::Threads
{
namespace
{
// One thread waits on the nearest deadline for every pending callback, so the
// cost of scheduling is a map entry rather than a sleeping thread. Firing is
// still a callAsync, which is what puts the callback on the message thread.
class CallAfterScheduler
{
public:
    CallAfterScheduler()
        : worker([this] { run(); })
    {
    }

    ~CallAfterScheduler()
    {
        {
            auto lock = std::lock_guard(mutex);
            running = false;
        }

        wake.notify_all();
        worker.join();
    }

    CallAfterScheduler(const CallAfterScheduler&) = delete;
    CallAfterScheduler& operator=(const CallAfterScheduler&) = delete;

    void schedule(Time::MS delay, Callback func)
    {
        {
            auto lock = std::lock_guard(mutex);
            pending.emplace(Clock::now() + std::chrono::milliseconds(delay.count),
                            std::move(func));
        }

        wake.notify_all();
    }

private:
    using Clock = std::chrono::steady_clock;

    void run()
    {
        auto lock = std::unique_lock(mutex);

        while (running)
        {
            if (pending.empty())
                wake.wait(lock);
            else
                wake.wait_until(lock, pending.begin()->first);

            dispatchDue(lock);
        }
    }

    void dispatchDue(std::unique_lock<std::mutex>& lock)
    {
        auto now = Clock::now();

        while (running && !pending.empty() && pending.begin()->first <= now)
        {
            auto func = std::move(pending.begin()->second);
            pending.erase(pending.begin());

            // Unlocked across the hand-off so a callback that schedules
            // another one does not deadlock on the way in.
            lock.unlock();
            callAsync(func);
            lock.lock();
        }
    }

    // Multi, because two callbacks can share a deadline to the nanosecond.
    std::multimap<Clock::time_point, Callback> pending;
    bool running = true;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
};
} // namespace

void callAfter(Time::MS delay, Callback func)
{
    if (delay.count <= 0)
    {
        callAsync(func);
        return;
    }

    Singleton::get<CallAfterScheduler>().schedule(delay, std::move(func));
}
} // namespace eacp::Threads
