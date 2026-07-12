#include "../Utils/WinInclude.h"

#include "EventLoop.h"
#include "ThreadUtils-Windows.h"
#include "../App/App.h"
#include "../Plugins/ModuleInfo.h"
#include "../Utils/Environment.h"
#include "../Utils/Singleton.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <tlhelp32.h>

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

    static const auto className =
        Plugins::getUniqueWindowClassName(L"EACPEventLoopMessageWindow");
    static auto classRegistered = false;

    if (!classRegistered)
    {
        auto wc = WNDCLASSEXW {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = messageWindowProc;
        wc.hInstance = (HINSTANCE) Plugins::getCurrentModuleHandle();
        wc.lpszClassName = className.c_str();
        classRegistered = RegisterClassExW(&wc) != 0;
    }

    messageWindow = CreateWindowExW(0,
                                    className.c_str(),
                                    L"",
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    HWND_MESSAGE,
                                    nullptr,
                                    (HINSTANCE) Plugins::getCurrentModuleHandle(),
                                    nullptr);
}

static void destroyMessageWindow()
{
    if (auto hwnd = messageWindow.exchange(nullptr))
        DestroyWindow(hwnd);
}

// An IDE like CLion, when NOT emulating a terminal, launches the app under a
// helper process and — on "Stop" — kills that helper rather than the app. That
// orphans the app: it gets no WM_CLOSE and no console event (a WebView app in
// particular just keeps running, since WebView2 keeps a message pump alive). So
// tie our lifetime to the launching process: when it exits, quit as if the
// window had been closed.
//
// Gated to non-distribution builds at the call site: a shipped app must never
// quit just because whatever launched it exited — an updater that launches-and-
// exits, or even Explorer restarting after a crash, would otherwise take it down.
static DWORD getParentProcessId()
{
    auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    auto entry = PROCESSENTRY32W {};
    entry.dwSize = sizeof(entry);
    auto self = GetCurrentProcessId();
    auto parent = DWORD {0};

    if (Process32FirstW(snapshot, &entry))
        do
        {
            if (entry.th32ProcessID == self)
            {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return parent;
}

static void installParentDeathWatchdog()
{
    auto parentPid = getParentProcessId();
    if (parentPid == 0)
        return;

    auto parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!parent)
        return;

    // If the parent has already exited (e.g. a spawn-and-exit launcher), the
    // process the IDE actually tracks is elsewhere — don't watch, or we'd quit a
    // legitimately-running app immediately.
    if (WaitForSingleObject(parent, 0) != WAIT_TIMEOUT)
    {
        CloseHandle(parent);
        return;
    }

    std::thread(
        [parent]
        {
            WaitForSingleObject(parent, INFINITE);
            CloseHandle(parent);
            // Same graceful quit the window's close button drives; callAsync
            // posts to the message-only window (thread-safe) so teardown runs on
            // the main thread.
            callAsync([] { getEventLoop().quit(); });
        })
        .detach();
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

    // Dev convenience only, never in a signed/shipped build (see the watchdog's
    // note): quit when the process that launched us exits, so an IDE "Stop" that
    // kills its launcher helper rather than the app still takes the app down.
    if (!Apps::isDistributionSigned())
        installParentDeathWatchdog();

    // Any callbacks queued before the loop existed will have been buffered
    // in PendingCallbacks; nudge the pump to drain them on the next
    // iteration.
    PostMessageW(messageWindow.load(), WM_EACP_RUN_PENDING, 0, 0);
}
} // namespace

void EventLoop::run()
{
    initLoopThread();

    // Loop ownership is advertised through the process environment so it
    // crosses eacp copies: a plugin-hosted app's quit reads it to know the
    // running loop is eacp's to stop (see stopProcessRootLoop). The thread
    // id rides along because WM_QUIT must target the pumping thread.
    setEnv("EACP_ROOT_LOOP", "1");
    setEnv("EACP_ROOT_LOOP_THREAD", std::to_string(GetCurrentThreadId()));

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

    setEnv("EACP_ROOT_LOOP", "0");
    mainThreadId = 0;
    destroyMessageWindow();

    // Tear down the dispatcher queue while COM is still healthy. If
    // we leave its WinRT smart pointers alive until static destruction
    // their Release runs after the apartment is gone and the process
    // crashes with STATUS_ACCESS_VIOLATION on exit (turning a passing
    // test run into a failure in CTest's eyes).
    shutdownMainThread();
}

bool EventLoop::runFor(Time::MS timeout)
{
    initLoopThread();

    runForDepth++;
    auto popDepth = std::shared_ptr<void> {nullptr, [](void*) { runForDepth--; }};

    auto deadline = Time::Deadline {timeout};
    auto timedOut = false;
    auto running = true;

    while (running)
    {
        if (deadline.expired())
        {
            timedOut = true;
            break;
        }

        auto remaining = deadline.remaining().count;

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

void stopProcessRootLoop()
{
    if (getEnvValue("EACP_ROOT_LOOP") != "1")
        return;

    auto thread = getEnvValue("EACP_ROOT_LOOP_THREAD");

    if (!thread.empty())
        PostThreadMessageW((DWORD) std::stoul(thread), WM_QUIT, 0, 0);
}

void EventLoop::call(Callback func)
{
    getPendingCallbacks().add(func);

    // Hosted copy (eacp inside a dlopen'd plugin — no run()/initLoopThread
    // ever happens): the first callAsync on the UI thread creates the
    // message-only window, whose messages the HOST's pump then dispatches
    // into this copy's messageWindowProc. Off-thread first use stays
    // buffered until attachCurrentThreadAsMain or a Window/EmbeddedView
    // brings the window up.
    if (messageWindow.load() == nullptr && isMainThread())
        ensureMessageWindow();

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

void scheduleStartup(const Callback& func)
{
    callAsync(func);
}

// Deliberately does NOT touch mainThreadId: that doubles as the
// loop-ownership marker quit()/stopEventLoop() post WM_QUIT through, and a
// hosted plugin must never be able to quit the host's loop.
void attachCurrentThreadAsMain()
{
    setCurrentThreadAsMainFallback();
    ensureMessageWindow();

    // Drain anything buffered before the wake channel existed.
    if (auto hwnd = messageWindow.load())
        PostMessageW(hwnd, WM_EACP_RUN_PENDING, 0, 0);
}

} // namespace eacp::Threads
