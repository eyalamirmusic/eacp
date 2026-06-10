#include "../Utils/WinInclude.h"

#include "EventLoop.h"
#include "ThreadUtils-Windows.h"
#include "../Utils/Singleton.h"

#include <eacp/Core/Utils/Containers.h>
#include <atomic>
#include <chrono>
#include <mutex>

namespace eacp::Threads
{

// Custom message that stopEventLoop posts to wake / break the
// innermost runFor. Independent of WM_QUIT (which signals "exit the
// whole process / outer run") so an inner pump can settle without
// tearing down the program.
constexpr UINT WM_EACP_STOP_LOOP = WM_APP + 0x42E0;

// Wake-up posted by EventLoop::call to make the pump drain its
// pending-callback queue. Posted to a dedicated message-only window
// (see messageWindow) rather than the thread or the WinRT
// DispatcherQueue: a window message is caught by our PeekMessage loop
// (so nested runFor pumps and co_awaited callAsync continuations still
// work) AND is dispatched by foreign modal loops — the move/size loop,
// menus, modal dialogs — so deferred work doesn't starve while one of
// those is on screen.
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
        auto fired = Vector<Callback> {};
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
    Vector<Callback> pendingCallbacks;
};

PendingCallbacks& getPendingCallbacks()
{
    return Singleton::get<PendingCallbacks>();
}

// Message-only window that EventLoop::call posts WM_EACP_RUN_PENDING to.
// Owned by the loop thread (created in initLoopThread, destroyed in run()'s
// teardown). Read from worker threads in EventLoop::call — PostMessage is
// thread-safe, so an atomic handle is enough.
static std::atomic<HWND> messageWindow {nullptr};

static LRESULT CALLBACK messageWindowProc(HWND hwnd,
                                          UINT msg,
                                          WPARAM wParam,
                                          LPARAM lParam)
{
    if (msg == WM_EACP_RUN_PENDING)
    {
        getPendingCallbacks().run();
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ensureMessageWindow()
{
    if (messageWindow.load())
        return;

    static const wchar_t* className = L"EACPEventLoopMessageWindow";
    static auto classRegistered = false;

    if (!classRegistered)
    {
        auto wc = WNDCLASSEXW {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = messageWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    messageWindow = CreateWindowExW(0,
                                    className,
                                    L"",
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    HWND_MESSAGE,
                                    nullptr,
                                    GetModuleHandleW(nullptr),
                                    nullptr);
}

static void destroyMessageWindow()
{
    if (auto hwnd = messageWindow.exchange(nullptr))
        DestroyWindow(hwnd);
}

namespace
{
void initLoopThread()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    initMainThread();
    mainThreadId = GetCurrentThreadId();
    ensureMessageWindow();

    // Any callbacks queued before the loop existed will have been buffered
    // in PendingCallbacks; nudge the pump to drain them on the next
    // iteration.
    PostMessageW(messageWindow.load(), WM_EACP_RUN_PENDING, 0, 0);
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
    destroyMessageWindow();

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
    auto popDepth = std::shared_ptr<void> {nullptr, [](void*) { runForDepth--; }};

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
            std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();

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

    // Wake the pump so it drains the callback list on the next tick.
    // Posting to our message-only window means the wake survives foreign
    // modal loops. Before the window exists we fall back to a thread
    // message; if neither is up yet the callback stays buffered and
    // initLoopThread() drains it once the loop starts.
    if (auto hwnd = messageWindow.load())
        PostMessageW(hwnd, WM_EACP_RUN_PENDING, 0, 0);
    else if (auto id = mainThreadId.load())
        PostThreadMessageW(id, WM_EACP_RUN_PENDING, 0, 0);
}

// Consumed by async callbacks that want to unblock the blocking caller
// of runEventLoopFor (e.g. an evaluateJavaScript completion handler
// signalling that the result is ready). When called from inside a
// nested runFor we only unwind that inner pump, leaving the outer
// run() alive so Apps::quit's queued stop still has a chance to run;
// when called from outside any runFor we PostQuitMessage so the
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
