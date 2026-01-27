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

    // Execute any callbacks that were queued before the dispatcher was ready
    auto queue = getDispatcherQueue();
    for (auto& cb : pendingCallbacks)
    {
        queue.TryEnqueue([cb = std::move(cb)]() { cb(); });
    }
    pendingCallbacks.clear();

    // DispatcherQueue on desktop still requires a Win32 message pump
    MSG msg;
    while (running && GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Clean shutdown of the dispatcher queue
    auto controller = getDispatcherQueueController();
    if (controller)
    {
        controller.ShutdownQueueAsync().get();
    }
}

void EventLoop::quit()
{
    running = false;

    auto queue = getDispatcherQueue();
    if (queue)
    {
        queue.TryEnqueue([]() { PostQuitMessage(0); });
    }
}

void EventLoop::call(Callback func)
{
    auto queue = getDispatcherQueue();
    if (queue)
    {
        queue.TryEnqueue([func = std::move(func)]() { func(); });
    }
    else
    {
        // Queue callbacks to be executed when the dispatcher is ready
        pendingCallbacks.push_back(std::move(func));
    }
}

} // namespace eacp::Threads
