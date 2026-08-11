#include <eacp/Core/Utils/WinInclude.h>

#include "DisplayLink.h"

#include <dcomp.h>

#include <atomic>
#include <thread>

namespace eacp::Threads
{
namespace
{
// Shared with the vblank thread and queued ticks, so a tick outliving the link
// fizzles instead of touching a dead callback.
struct TickState
{
    explicit TickState(const Callback& cbToUse)
        : cb(cbToUse)
    {
    }

    Callback cb;
    std::atomic<bool> alive {true};
    std::atomic<bool> pending {false};
};

// dcomp.h only declares this at the Windows 11 SDK level, so resolve it
// dynamically to keep building and running on Windows 10.
using WaitForCompositorClockFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

WaitForCompositorClockFn loadCompositorClock(HMODULE dcomp)
{
    if (dcomp == nullptr)
        return nullptr;

    return reinterpret_cast<WaitForCompositorClockFn>(
        GetProcAddress(dcomp, "DCompositionWaitForCompositorClock"));
}
} // namespace

// Waits for the DWM compositor clock on a dedicated thread and posts ticks to
// the main thread. Ticks coalesce rather than piling up behind a busy main
// thread.
struct DisplayLink::Native
{
    explicit Native(const Callback& cb)
        : state(std::make_shared<TickState>(cb))
    {
        assertMainThread();

        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        dcomp = LoadLibraryW(L"dcomp.dll");
        waitForClock = loadCompositorClock(dcomp);
        thread = std::thread([this] { waitLoop(); });
    }

    ~Native()
    {
        assertMainThread();

        state->alive = false;
        SetEvent(stopEvent);
        thread.join();
        CloseHandle(stopEvent);

        if (dcomp != nullptr)
            FreeLibrary(dcomp);
    }

    void waitLoop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        while (waitForNextTick())
            postTick();
    }

    // Blocks until the next vblank; false once the stop event is signalled.
    bool waitForNextTick() const
    {
        if (waitForClock != nullptr)
        {
            auto wait = waitForClock(1, &stopEvent, INFINITE);

            if (wait == WAIT_OBJECT_0)
                return false;

            if (wait == WAIT_OBJECT_0 + 1)
                return true;
        }

        // No compositor clock: degrade to a fixed ~60 Hz cadence.
        return WaitForSingleObject(stopEvent, 16) == WAIT_TIMEOUT;
    }

    void postTick() const
    {
        if (state->pending.exchange(true))
            return;

        callAsync(
            [state = state]
            {
                state->pending = false;

                if (state->alive)
                    state->cb();
            });
    }

    std::shared_ptr<TickState> state;
    HANDLE stopEvent = nullptr;
    HMODULE dcomp = nullptr;
    WaitForCompositorClockFn waitForClock = nullptr;
    std::thread thread;
};

DisplayLink::DisplayLink(const FrameCallback& cb)
    : callback(timedTick(cb))
    , impl(callback)
{
}

} // namespace eacp::Threads
