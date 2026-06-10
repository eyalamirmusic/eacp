#include <eacp/Core/Utils/WinInclude.h>

#include "DisplayLink.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Threads/ThreadUtils.h>

#include <dcomp.h>

#include <atomic>
#include <memory>
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
        thread = std::thread([this] { waitLoop(); });
    }

    ~Native()
    {
        assertMainThread();

        state->alive = false;
        SetEvent(stopEvent);
        thread.join();
        CloseHandle(stopEvent);
    }

    void waitLoop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        while (true)
        {
            auto wait = DCompositionWaitForCompositorClock(1, &stopEvent, INFINITE);

            if (wait == WAIT_OBJECT_0)
                return;

            // No compositor clock in this session: degrade to a fixed
            // ~60 Hz cadence, still honouring the stop event.
            if (wait != WAIT_OBJECT_0 + 1)
                if (WaitForSingleObject(stopEvent, 16) != WAIT_TIMEOUT)
                    return;

            postTick();
        }
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
    std::thread thread;
};

DisplayLink::DisplayLink(const FrameCallback& cb)
    : callback(timedTick(cb))
    , impl(callback)
{
}

} // namespace eacp::Threads
