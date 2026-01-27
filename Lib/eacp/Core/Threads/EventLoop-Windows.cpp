#include "EventLoop.h"
#include "ThreadUtils-Windows.h"

#include <winrt/Windows.Foundation.h>

#include <vector>

namespace eacp::Threads
{

static bool running = false;
static std::vector<Callback> pendingCallbacks;

void EventLoop::run()
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    initMainThread();
    running = true;

    auto queue = getDispatcherQueue();

    for (auto& cb : pendingCallbacks)
        queue.TryEnqueue(cb);

    pendingCallbacks.clear();

    auto msg = MSG();

    while (running && GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (auto controller = getDispatcherQueueController())
        controller.ShutdownQueueAsync().get();
}

void EventLoop::quit()
{
    running = false;

    if (auto queue = getDispatcherQueue())
        queue.TryEnqueue([] { PostQuitMessage(0); });
}

void EventLoop::call(Callback func)
{
    if (auto queue = getDispatcherQueue())
        queue.TryEnqueue([func = std::move(func)]() { func(); });
    else
        pendingCallbacks.push_back(std::move(func));
}

} // namespace eacp::Threads
