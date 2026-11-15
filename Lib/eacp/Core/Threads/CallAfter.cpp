#include "EventLoop.h"
#include "CallAfter.h"
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
//
// The thread starts with the first callback and stop() joins it again, so a
// hosted copy can be left with no thread of its own before its image unloads;
// the next schedule starts another.
class CallAfterScheduler
{
public:
    CallAfterScheduler() = default;

    ~CallAfterScheduler() { stop(); }

    CallAfterScheduler(const CallAfterScheduler&) = delete;
    CallAfterScheduler& operator=(const CallAfterScheduler&) = delete;

    void schedule(Time::MS delay, Callback func)
    {
        {
            auto lock = std::lock_guard(mutex);
            pending.emplace(Clock::now() + std::chrono::milliseconds(delay.count),
                            std::move(func));

            if (!worker.joinable())
            {
                running = true;
                worker = std::thread([this] { run(); });
            }
        }

        wake.notify_all();
    }

    void stop()
    {
        {
            auto lock = std::lock_guard(mutex);
            running = false;
            pending.clear();
        }

        wake.notify_all();

        if (worker.joinable())
            worker.join();
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
            {
                // A copy: wait_until reads its deadline again after waking, and
                // stop() may have freed the map node it lived in by then.
                auto deadline = pending.begin()->first;
                wake.wait_until(lock, deadline);
            }

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
    bool running = false;
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

void stopCallAfterScheduler()
{
    Singleton::get<CallAfterScheduler>().stop();
}
} // namespace eacp::Threads
