#include "../Utils/WinInclude.h"

#include "ThreadUtils-Windows.h"

#include <atomic>
#include <cassert>

namespace eacp::Threads
{

// Captured at static-init time — for an executable that runs on the main thread
// before main(), and for a dlopen'd plugin on whichever thread the host loaded it
// (usually, but not guaranteed, its UI thread). initMainThread() overwrites it
// with the thread that actually owns the loop; setCurrentThreadAsMainFallback
// lets a hosted plugin correct it from a call known to be on the UI thread.
static std::atomic<DWORD> mainThreadId {GetCurrentThreadId()};

void setCurrentThreadAsMainFallback()
{
    mainThreadId = GetCurrentThreadId();
}

void initMainThread()
{
    // Suppress Windows Error Reporting + the JIT-debugger pop-up that otherwise
    // interrupts every test run any time WebView2 trips an access violation
    // during DLL detach at shutdown. Lives here — the loop owner's bootstrap —
    // rather than at static init, so merely dlopen-loading an eacp plugin doesn't
    // overwrite the host process's error mode.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                 | SEM_NOOPENFILEERRORBOX);

    mainThreadId = GetCurrentThreadId();
}

// Nothing to tear down any more: the DispatcherQueue controller this used to
// release (before the COM apartment died under it) no longer exists.
void shutdownMainThread() {}

bool isMainThread()
{
    return GetCurrentThreadId() == mainThreadId.load();
}

void assertMainThread()
{
    assert(isMainThread() && "Must be accessed from Main Thread");
}

} // namespace eacp::Threads
