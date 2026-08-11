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

// Breaks the innermost runFor. Separate from WM_QUIT — which exits the whole
// process — so an inner pump can settle without tearing the program down.
constexpr UINT WM_EACP_STOP_LOOP = WM_APP + 0x42E0;

// Drains the pending-callback queue. Goes to a message-only window rather than
// the thread queue, so foreign modal loops (move/size, menus, dialogs) dispatch
// it too and deferred work doesn't starve while one is on screen.
constexpr UINT WM_EACP_RUN_PENDING = WM_APP + 0x42E1;

struct EventLoopState
{
    // Loop-ownership marker: non-zero only while run()/runFor owns the pump, so
    // a hosted plugin (which never sets it) can't quit the host's loop.
    std::atomic<DWORD> loopThreadId {0};

    // Owned by the loop thread, but read from worker threads in EventLoop::call.
    // PostMessage is thread-safe, so an atomic handle is enough.
    std::atomic<HWND> messageWindow {nullptr};
};

static EventLoopState& getEventLoopState()
{
    return Singleton::get<EventLoopState>();
}

static thread_local int runForDepth = 0;

struct PendingCallbacks
{
    void run()
    {
        // Snapped under the lock and run outside it, so a callAsync from inside
        // a callback lands in the next pass rather than deadlocking.
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
    if (getEventLoopState().messageWindow.load())
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

    getEventLoopState().messageWindow =
        CreateWindowExW(0,
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
    if (auto hwnd = getEventLoopState().messageWindow.exchange(nullptr))
        DestroyWindow(hwnd);
}

// An IDE that kills its launcher helper rather than the app on "Stop" orphans
// the app, so tie our lifetime to the launching process. Gated to
// non-distribution builds at the call site.
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

    // An already-exited parent means the IDE tracks some other process; watching
    // it would quit a legitimately-running app immediately.
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
            // Via callAsync, so teardown runs on the main thread.
            callAsync([] { getEventLoop().quit(); });
        })
        .detach();
}

namespace
{
void initLoopThread()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // A single-threaded apartment, as WebView2, the shell dialogs and
    // DirectComposition all expect.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    initMainThread();
    getEventLoopState().loopThreadId = GetCurrentThreadId();
    ensureMessageWindow();

    // Dev convenience only: a shipped app must never quit merely because
    // whatever launched it exited.
    if (!Apps::isDistributionSigned())
        installParentDeathWatchdog();

    // Drain callbacks buffered before the loop existed.
    PostMessageW(
        getEventLoopState().messageWindow.load(), WM_EACP_RUN_PENDING, 0, 0);
}
} // namespace

void EventLoop::run()
{
    initLoopThread();

    // Advertised through the process environment so it crosses eacp copies (see
    // stopProcessRootLoop). WM_QUIT must target the pumping thread, hence the id.
    setEnv("EACP_ROOT_LOOP", "1");
    setEnv("EACP_ROOT_LOOP_THREAD", std::to_string(GetCurrentThreadId()));

    // Exits only on WM_QUIT, never on WM_EACP_STOP_LOOP, so a nested runFor can
    // settle without taking the process down with it.
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
    getEventLoopState().loopThreadId = 0;
    destroyMessageWindow();

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
                // Re-posted so the outer run() sees it too.
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
    // Nested runFor calls see the WM_QUIT too and unwind, re-posting it on their
    // way out so the outer run still gets it.
    auto id = getEventLoopState().loopThreadId.load();
    if (id != 0)
        PostThreadMessageW(id, WM_QUIT, 0, 0);
}

bool isEventLoopRunning()
{
    return runForDepth > 0 || getEnvValue("EACP_ROOT_LOOP") == "1";
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

    // A hosted copy never runs initLoopThread, so the first main-thread
    // callAsync creates the window the host's pump then dispatches into.
    if (getEventLoopState().messageWindow.load() == nullptr && isMainThread())
        ensureMessageWindow();

    // Falls back to a thread message before the window exists; with neither up
    // the callback stays buffered until initLoopThread() drains it.
    if (auto hwnd = getEventLoopState().messageWindow.load())
        PostMessageW(hwnd, WM_EACP_RUN_PENDING, 0, 0);
    else if (auto id = getEventLoopState().loopThreadId.load())
        PostThreadMessageW(id, WM_EACP_RUN_PENDING, 0, 0);
}

// Inside a nested runFor this unwinds only that inner pump, leaving the outer
// run() alive; outside any runFor it quits the outer run.
void stopEventLoop()
{
    auto id = getEventLoopState().loopThreadId.load();
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

// Deliberately does NOT set loopThreadId: a hosted plugin must never be able to
// quit the host's loop.
void attachCurrentThreadAsMain()
{
    setCurrentThreadAsMainFallback();
    ensureMessageWindow();

    // Drain anything buffered before the wake channel existed.
    if (auto hwnd = getEventLoopState().messageWindow.load())
        PostMessageW(hwnd, WM_EACP_RUN_PENDING, 0, 0);
}

} // namespace eacp::Threads
