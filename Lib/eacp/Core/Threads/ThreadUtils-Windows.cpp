#include "../Utils/WinInclude.h"

#include "ThreadUtils-Windows.h"
#include "../Utils/Singleton.h"

#include <atomic>
#include <cassert>

namespace eacp::Threads
{

// Captured at construction, which for a dlopen'd plugin is whichever thread the
// host loaded it on. initMainThread() overwrites it with the loop's own thread.
struct MainThreadState
{
    std::atomic<DWORD> mainThreadId {GetCurrentThreadId()};
};

static MainThreadState& getMainThreadState()
{
    return Singleton::get<MainThreadState>();
}

// Forces the capture above at static-init time, not lazily on whichever thread
// happens to query first.
[[maybe_unused]] static MainThreadState& mainThreadStateInit = getMainThreadState();

void setCurrentThreadAsMainFallback()
{
    getMainThreadState().mainThreadId = GetCurrentThreadId();
}

void initMainThread()
{
    // Suppresses Windows Error Reporting and the JIT-debugger pop-up. Set from
    // the loop owner's bootstrap rather than static init, so dlopen-loading an
    // eacp plugin doesn't overwrite the host process's error mode.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
                 | SEM_NOOPENFILEERRORBOX);

    getMainThreadState().mainThreadId = GetCurrentThreadId();
}

void shutdownMainThread() {}

bool isMainThread()
{
    return GetCurrentThreadId() == getMainThreadState().mainThreadId.load();
}

void assertMainThread()
{
    assert(isMainThread() && "Must be accessed from Main Thread");
}

} // namespace eacp::Threads
