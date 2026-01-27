#include "ThreadUtils-Windows.h"
#include <DispatcherQueue.h>
#include <winrt/Windows.Foundation.h>

namespace eacp::Threads
{

static System::DispatcherQueueController dispatcherController {nullptr};
static System::DispatcherQueue dispatcherQueue {nullptr};

void initMainThread()
{
    auto options = DispatcherQueueOptions(
        sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_ASTA);

    ABI::Windows::System::IDispatcherQueueController* controller = nullptr;
    auto hr = CreateDispatcherQueueController(options, &controller);

    if (SUCCEEDED(hr) && controller)
    {
        winrt::attach_abi(dispatcherController, controller);
        dispatcherQueue = dispatcherController.DispatcherQueue();
    }
}

System::DispatcherQueue getDispatcherQueue()
{
    return dispatcherQueue;
}

System::DispatcherQueueController getDispatcherQueueController()
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
