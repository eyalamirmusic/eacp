#include "../Utils/WinInclude.h"

#include "EventLoop.h"
#include "ThreadUtils-Windows.h"
#include "../Utils/Singleton.h"

#include <ea_data_structures/Structures/Vector.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace eacp::Threads
{

// Custom message that stopEventLoop posts to wake / break the
// innermost runFor. Independent of WM_QUIT (which signals "exit the
// whole process / outer run") so an inner pump can settle without
// tearing down the program.
constexpr UINT WM_EACP_STOP_LOOP = WM_APP + 0x42E0;

// Wake-up posted by EventLoop::call to make the pump drain its
// pending-callback queue. We don't use the WinRT DispatcherQueue
// for this — under nested pumping its messages aren't reliably
// processed by our PeekMessage loop (the items sit in the queue and
// only fire after the outer pump exits), which breaks any coroutine
// that co_awaits a callAsync continuation while running inside a
// nested runEventLoopFor.
constexpr UINT WM_EACP_RUN_PENDING = WM_APP + 0x42E1;

static std::atomic<DWORD> mainThreadId {0};
static thread_local int runForDepth = 0;

struct PendingCallbacks
{
    void run()
    {
        // Snap the queue under the lock, then run callbacks outside it
        // so user code can re-enter (e.g. callAsync from inside a
        // callback won't deadlock on the recursive_mutex but also
        // won't be processed in this pass).
        auto fired = std::vector<Callback> {};
        {
            auto guard = std::lock_guard {mutex};
            fired.assign(pendingCallbacks.begin(), pendingCallbacks.end());
            pendingCallbacks.clear();
        }
        for (auto& cb: fired)
            cb();
    }

    void add(const Callback& cb)
    {
        auto guard = std::lock_guard {mutex};
        pendingCallbacks.add(cb);
    }

    std::recursive_mutex mutex;
    EA::Vector<Callback> pendingCallbacks;
};

PendingCallbacks& getPendingCallbacks()
{
    return Singleton::get<PendingCallbacks>();
}

namespace
{
void initLoopThread()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    initMainThread();
    mainThreadId = GetCurrentThreadId();

    // Any callbacks queued before the main thread was identified will
    // have been buffered in PendingCallbacks; nudge the pump to drain
    // them on the next iteration.
    PostThreadMessageW(mainThreadId.load(), WM_EACP_RUN_PENDING, 0, 0);
}
} // namespace

void EventLoop::run()
{
    initLoopThread();

    // Outer run exits only on WM_QUIT — never on WM_EACP_STOP_LOOP, so
    // nested runFor calls can settle without taking the whole process
    // down with them.
    while (true)
    {
        auto msg = MSG();
        auto result = GetMessage(&msg, nullptr, 0, 0);

        if (result == 0 || result == -1)
            break;

        if (msg.message == WM_EACP_RUN_PENDING)
        {
            getPendingCallbacks().run();
            continue;
        }

        if (msg.message == WM_EACP_STOP_LOOP)
            continue;

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    mainThreadId = 0;

    // Tear down the dispatcher queue while COM is still healthy. If
    // we leave its WinRT smart pointers alive until static destruction
    // their Release runs after the apartment is gone and the process
    // crashes with STATUS_ACCESS_VIOLATION on exit (turning a passing
    // test run into a failure in CTest's eyes).
    shutdownMainThread();
}

bool EventLoop::runFor(std::chrono::milliseconds timeout)
{
    initLoopThread();

    runForDepth++;
    auto popDepth = std::shared_ptr<void> {nullptr,
                                            [](void*) { runForDepth--; }};

    auto deadline = std::chrono::steady_clock::now() + timeout;
    auto timedOut = false;
    auto running = true;

    while (running)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            timedOut = true;
            break;
        }

        auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now)
                .count();

        auto wait = MsgWaitForMultipleObjectsEx(
            0, nullptr, (DWORD) remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        if (wait == WAIT_TIMEOUT)
        {
            timedOut = true;
            break;
        }

        auto msg = MSG();
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                // Re-post so the outer run() also sees it and shuts the
                // process down cleanly; meanwhile break out of this
                // inner pump.
                PostQuitMessage(static_cast<int>(msg.wParam));
                running = false;
                break;
            }

            if (msg.message == WM_EACP_RUN_PENDING)
            {
                getPendingCallbacks().run();
                continue;
            }

            if (msg.message == WM_EACP_STOP_LOOP)
            {
                running = false;
                break;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return !timedOut;
}

void EventLoop::quit()
{
    // Outer-run quit: post WM_QUIT so GetMessage in run() returns 0
    // and the loop exits. Inner runFor calls will also see it (via
    // PeekMessage) and unwind, re-posting WM_QUIT on their way out so
    // the outer run still gets it.
    auto id = mainThreadId.load();
    if (id != 0)
        PostThreadMessageW(id, WM_QUIT, 0, 0);
}

void EventLoop::call(Callback func)
{
    getPendingCallbacks().add(func);

    // Poke the pump so it drains the callback list on the next tick.
    // If the main thread hasn't entered the loop yet (mainThreadId
    // still 0) the callback stays buffered and gets drained by
    // initLoopThread()'s same WM_EACP_RUN_PENDING post.
    auto id = mainThreadId.load();
    if (id != 0)
        PostThreadMessageW(id, WM_EACP_RUN_PENDING, 0, 0);
}

// Consumed by async callbacks that want to unblock the blocking caller
// of runEventLoopFor (e.g. an evaluateJavaScript completion handler
// signalling that the result is ready). When called from inside a
// nested runFor we only unwind that inner pump, leaving the outer
// run() alive so Apps::quit's queued teardown still has a chance to
// run; when called from outside any runFor we PostQuitMessage so the
// outer run exits.
void stopEventLoop()
{
    auto id = mainThreadId.load();
    if (id == 0)
        return;

    if (runForDepth > 0)
        PostThreadMessageW(id, WM_EACP_STOP_LOOP, 0, 0);
    else
        PostThreadMessageW(id, WM_QUIT, 0, 0);
}

} // namespace eacp::Threads
