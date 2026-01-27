#include "EventLoop.h"
#include "ThreadUtils-Windows.h"

#include <winrt/Windows.Foundation.h>

namespace eacp::Threads
{

static bool running = false;

void EventLoop::run()
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    initMainThread();
    running = true;

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
}

} // namespace eacp::Threads
