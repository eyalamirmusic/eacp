#include "EventLoop.h"
#include "ThreadUtils-Windows.h"
#include <vector>
#include <atomic>
#include <mutex>
#include "../Utils/Singleton.h"

namespace eacp::Threads
{

static std::atomic running {false};

struct PendingCallbacks
{
    void run()
    {
        auto guard = std::lock_guard(mutex);
        auto queue = getDispatcherQueue();

        for (auto& cb : pendingCallbacks)
            queue.TryEnqueue(cb);

        pendingCallbacks.clear();
    }

    void add(const Callback& cb)
    {
        auto guard = std::lock_guard(mutex);
        pendingCallbacks.push_back(cb);
    }

    std::recursive_mutex mutex;
    std::vector<Callback> pendingCallbacks;
};

PendingCallbacks& getPendingCallbacks() {return Singleton::get<PendingCallbacks>();}

void EventLoop::run()
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    initMainThread();
    getPendingCallbacks().run();

    running = true;

    while (running)
    {
        auto msg = MSG();

        if (GetMessage(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            quit();
        }
    }
}

void EventLoop::quit()
{
    running = false;
}

void EventLoop::call(Callback func)
{
    if (auto queue = getDispatcherQueue())
        queue.TryEnqueue([func] { func(); });
    else
        getPendingCallbacks().add(func);
}

} // namespace eacp::Threads
