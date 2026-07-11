#include "../Utils/WinInclude.h"

#include "ThreadUtils-Windows.h"
#include <DispatcherQueue.h>
#include <winrt/Windows.Foundation.h>

#include <atomic>
#include <cstdio>

namespace eacp::Threads
{

static System::DispatcherQueueController dispatcherController {nullptr};
static System::DispatcherQueue dispatcherQueue {nullptr};

// Captured at static-init time — for an executable that runs on the main
// thread before main(), and for a dlopen'd plugin on whichever thread the
// host loaded it (usually, but not guaranteed, its UI thread). Used as a
// fallback for isMainThread() when the dispatcher queue hasn't been
// initialized; re-seedable via setCurrentThreadAsMainFallback so a hosted
// plugin can correct it from a call that is known to be on the UI thread.
static std::atomic<DWORD> fallbackMainThreadId {GetCurrentThreadId()};

void setCurrentThreadAsMainFallback()
{
    fallbackMainThreadId = GetCurrentThreadId();
}

void initMainThread()
{
    if (dispatcherQueue)
        return;

    // Suppress Windows Error Reporting + the JIT-debugger pop-up that
    // otherwise interrupts every test run any time WebView2 / WinRT trips
    // an access violation during DLL detach at shutdown. Lives here — the
    // loop owner's bootstrap — rather than at static init, so merely
    // dlopen-loading an eacp plugin doesn't overwrite the host process's
    // error mode.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                 | SEM_NOOPENFILEERRORBOX);

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

    // Pre-dispatcher fallback: compare against the captured (or re-seeded)
    // main-thread id.
    return GetCurrentThreadId() == fallbackMainThreadId.load();
}

void assertMainThread()
{
    assert(isMainThread() && "Must be accessed from Main Thread");
}

} // namespace eacp::Threads
