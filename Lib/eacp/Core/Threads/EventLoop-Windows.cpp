#include "EventLoop.h"
#include "ThreadUtils-Windows.h"
#include "../Utils/Singleton.h"
#include "../Utils/Windows.h"

#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cassert>

namespace eacp::Threads
{

static std::atomic running {false};
static std::atomic<DWORD> mainThreadId {0};

struct PendingCallbacks
{
    void run()
    {
        auto guard = std::lock_guard(mutex);
        auto queue = getDispatcherQueue();

        for (auto& cb: pendingCallbacks)
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

PendingCallbacks& getPendingCallbacks()
{
    return Singleton::get<PendingCallbacks>();
}

namespace
{
void initLoopThread()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    initMainThread();
    mainThreadId = GetCurrentThreadId();
    getPendingCallbacks().run();
}
} // namespace

void EventLoop::run()
{
    initLoopThread();

    running = true;

    while (running)
    {
        auto msg = MSG();

        auto result = GetMessage(&msg, nullptr, 0, 0);
        if (result == 0 || result == -1)
        {
            quit();
            break;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    mainThreadId = 0;
}

bool EventLoop::runFor(std::chrono::milliseconds timeout)
{
    initLoopThread();

    running = true;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    auto timedOut = false;

    while (running)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            timedOut = true;
            break;
        }

        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now)
                .count();

        auto wait = MsgWaitForMultipleObjectsEx(
            0, nullptr, (DWORD) remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        if (wait == WAIT_TIMEOUT)
        {
            timedOut = true;
            break;
        }

        auto msg = MSG();
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                running = false;
                break;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    mainThreadId = 0;
    return !timedOut;
}

void EventLoop::quit()
{
    running = false;

    auto id = mainThreadId.load();
    if (id != 0)
        PostThreadMessageW(id, WM_NULL, 0, 0);
}

void EventLoop::call(Callback func)
{
    if (auto queue = getDispatcherQueue())
        queue.TryEnqueue([func] { func(); });
    else
        getPendingCallbacks().add(func);
}

} // namespace eacp::Threads
