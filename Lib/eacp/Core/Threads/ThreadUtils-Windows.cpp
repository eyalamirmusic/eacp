#include "ThreadUtils-Windows.h"

#include <winrt/Windows.Foundation.h>

namespace eacp::Threads
{

static winrt::Windows::System::DispatcherQueueController dispatcherController {nullptr};
static winrt::Windows::System::DispatcherQueue dispatcherQueue {nullptr};

void initMainThread()
{
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

winrt::Windows::System::DispatcherQueue getDispatcherQueue()
{
    return dispatcherQueue;
}

winrt::Windows::System::DispatcherQueueController getDispatcherQueueController()
{
    return dispatcherController;
}

bool isMainThread()
{
    if (dispatcherQueue)
    {
        return dispatcherQueue.HasThreadAccess();
    }
    return false;
}

void assertMainThread()
{
    assert(isMainThread() && "Must be accessed from Main Thread");
}

} // namespace eacp::Threads
