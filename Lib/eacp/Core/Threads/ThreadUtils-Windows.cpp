#include "../Utils/WinInclude.h"

#include "ThreadUtils-Windows.h"
#include <DispatcherQueue.h>
#include <winrt/Windows.Foundation.h>

#include <cstdio>

namespace eacp::Threads
{

static System::DispatcherQueueController dispatcherController {nullptr};
static System::DispatcherQueue dispatcherQueue {nullptr};

// Captured at static-init time, which on Windows runs on the main
// thread before main(). Used as a fallback for isMainThread() when
// the dispatcher queue hasn't been initialized yet (e.g. tests that
// call callAsync before any runEventLoop / runFor has bootstrapped
// the DispatcherQueue).
static const DWORD initialMainThreadId = GetCurrentThreadId();

// Suppress Windows Error Reporting + the JIT-debugger pop-up that
// otherwise interrupts every test run any time WebView2 / WinRT trips
// an access violation during DLL detach at shutdown. The actual crash
// is intercepted by the vectored exception handler in test main; this
// only stops the OS from also showing a dialog before that runs.
// Runs at static-init on the main thread, so the suppression is in
// place before any test code (or app code) executes.
namespace
{
const auto crashDialogSuppression = []
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                 | SEM_NOOPENFILEERRORBOX);
    return 0;
}();
} // namespace

void initMainThread()
{
    if (dispatcherQueue)
        return;

    // DQTAT_COM_NONE: caller (initLoopThread / our test main()) has
    // already winrt::init_apartment'd this thread, so the dispatcher
    // queue must not try to (re)initialize COM with a different
    // threading mode. Asking for DQTAT_COM_ASTA when COM is already
    // STA returns RPC_E_CHANGED_MODE and leaves the queue uninitialized.
    auto options = DispatcherQueueOptions(
        sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE);

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

void shutdownMainThread()
{
    dispatcherQueue = nullptr;
    dispatcherController = nullptr;
}

bool isMainThread()
{
    if (dispatcherQueue)
        return dispatcherQueue.HasThreadAccess();

    // Pre-dispatcher fallback: just compare the thread that loaded
    // this TU (always the main thread on Windows) against the caller.
    return GetCurrentThreadId() == initialMainThreadId;
}

void assertMainThread()
{
    assert(isMainThread() && "Must be accessed from Main Thread");
}

} // namespace eacp::Threads
