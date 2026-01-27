#include "ThreadUtils-Windows.h"
#include <atomic>

#include <winrt/Windows.Foundation.h>

namespace eacp::Threads
{

static std::atomic<DWORD> mainThreadId {0};
static winrt::Windows::System::DispatcherQueueController dispatcherController {nullptr};
static winrt::Windows::System::DispatcherQueue dispatcherQueue {nullptr};

void initMainThread()
{
    mainThreadId = GetCurrentThreadId();

    DispatcherQueueOptions options {sizeof(DispatcherQueueOptions),
                                    DQTYPE_THREAD_CURRENT,
                                    DQTAT_COM_ASTA};

    ABI::Windows::System::IDispatcherQueueController* controller = nullptr;
    auto hr = CreateDispatcherQueueController(options, &controller);

    if (SUCCEEDED(hr) && controller)
    {
        winrt::attach_abi(dispatcherController, controller);
        dispatcherQueue = dispatcherController.DispatcherQueue();
    }
}

DWORD getMainThreadID()
{
    return mainThreadId;
}

winrt::Windows::System::DispatcherQueue getDispatcherQueue()
{
    return dispatcherQueue;
}

bool isMainThread()
{
    if (dispatcherQueue)
    {
        return dispatcherQueue.HasThreadAccess();
    }
    return GetCurrentThreadId() == mainThreadId;
}

void assertMainThread()
{
    assert(isMainThread() && "Must be accessed from Main Thread");
}

} // namespace eacp::Threads
