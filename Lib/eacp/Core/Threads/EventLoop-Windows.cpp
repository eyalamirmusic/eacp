#include "EventLoop.h"
#include "ThreadUtils-Windows.h"

#include <queue>
#include <mutex>

namespace eacp::Threads
{

static constexpr UINT WM_EACP_CALLBACK = WM_USER + 1;
static std::queue<Callback> callbackQueue;
static std::mutex callbackMutex;
static bool running = false;

static void processCallbacks()
{
    std::queue<Callback> toProcess;
    {
        auto lock = std::lock_guard (callbackMutex);
        std::swap(toProcess, callbackQueue);
    }

    while (!toProcess.empty())
    {
        auto cb = std::move(toProcess.front());
        toProcess.pop();
        cb();
    }
}

void EventLoop::run()
{
    initMainThread();
    running = true;
    processCallbacks();

    MSG msg;
    while (running && GetMessage(&msg, NULL, 0, 0))
    {
        if (msg.message == WM_EACP_CALLBACK)
        {
            processCallbacks();
        }
        else
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}

void EventLoop::quit()
{
    running = false;
    PostThreadMessage(getMainThreadID(), WM_QUIT, 0, 0);
}

void EventLoop::call(Callback func)
{
    {
        auto lock = std::lock_guard(callbackMutex);
        callbackQueue.push(std::move(func));
    }

    if (getMainThreadID() != 0)
    {
        PostThreadMessage(getMainThreadID(), WM_EACP_CALLBACK, 0, 0);
    }
}

} // namespace eacp::Threads
