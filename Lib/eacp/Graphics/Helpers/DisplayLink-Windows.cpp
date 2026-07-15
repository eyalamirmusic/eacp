#include <eacp/Core/Utils/WinInclude.h>

#include "DisplayLink.h"

#include <dcomp.h>

#include <atomic>
#include <thread>

namespace eacp::Threads
{
namespace
{
// Shared between the vblank thread, ticks already queued on the main thread
// and the link itself, so a tick still in the event queue when the link is
// destroyed fizzles instead of touching a dead callback.
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

// DCompositionWaitForCompositorClock ships in dcomp.dll on Windows 10 1803+,
// but dcomp.h only declares it when targeting the Windows 11 (Cobalt) SDK
// level, so we resolve it dynamically to keep building — and running — on
// Windows 10 without it.
using WaitForCompositorClockFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

WaitForCompositorClockFn loadCompositorClock(HMODULE dcomp)
{
    if (dcomp == nullptr)
        return nullptr;

    return reinterpret_cast<WaitForCompositorClockFn>(
        GetProcAddress(dcomp, "DCompositionWaitForCompositorClock"));
}
} // namespace

// Waits for the DWM compositor clock on a dedicated thread and posts each
// tick to the main thread, lining callbacks up with the vsync that
// composition swapchains present against. Ticks coalesce: while one is still
// queued behind a busy main thread, further vblanks are skipped rather than
// piling up.
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

    // Blocks until the next vblank, returning false once the stop event is
    // signalled so the loop ends.
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

        // No compositor clock (older Windows 10, or none in this session):
        // degrade to a fixed ~60 Hz cadence, still honouring the stop event.
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
