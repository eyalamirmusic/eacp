// A plug-in DLL linking its own eacp copy posts WM_EACP_RUN_PENDING to its own
// message-only window. An eacp host pumping the thread must dispatch it there
// rather than take it for its own. A window of this file stands in for the
// other copy's.
//
// The parent-death watchdog the loop installs: once per attach, quitting the
// loop when the process that started it exits, taken down by a detach, and
// starting no thread through the C runtime -- in a short-lived program built
// with the static runtime, such a thread can still be starting at exit and
// enter the runtime after it has shut down. The cases that need a parent they
// control run LoopExitHarness as a child.

#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <chrono>
#include <cstdio>

#include <tlhelp32.h>

using namespace nano;

namespace EventLoopWindowsTests
{
namespace
{
// WM_EACP_RUN_PENDING, file-local in EventLoop-Windows.cpp. Keep the two in step.
constexpr auto runPendingMessage = UINT {WM_APP + 0x42E1};

constexpr auto otherCopyClassName = L"EACPEventLoopTestOtherCopyWindow";

auto runPendingReceived = 0;

constexpr auto repeatedRunForPasses = 20;

int countThreadsInThisProcess()
{
    auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return -1;

    auto entry = THREADENTRY32 {};
    entry.dwSize = sizeof(entry);
    auto self = GetCurrentProcessId();
    auto threadCount = 0;

    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == self)
                ++threadCount;
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return threadCount;
}

// Each watchdog install opens the parent's process handle, so a second one
// shows here even though a thread-pool wait starts no thread.
int countHandlesInThisProcess()
{
    auto count = DWORD {};
    GetProcessHandleCount(GetCurrentProcess(), &count);
    return (int) count;
}

LRESULT CALLBACK otherCopyWindowProc(HWND hwnd,
                                     UINT message,
                                     WPARAM wParam,
                                     LPARAM lParam)
{
    if (message == runPendingMessage)
    {
        ++runPendingReceived;
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

struct OtherCopyWindow
{
    OtherCopyWindow()
    {
        auto windowClass = WNDCLASSEXW {};
        windowClass.cbSize = sizeof(WNDCLASSEXW);
        windowClass.lpfnWndProc = otherCopyWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = otherCopyClassName;
        registered = RegisterClassExW(&windowClass) != 0;

        hwnd = CreateWindowExW(0,
                               otherCopyClassName,
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

    ~OtherCopyWindow()
    {
        if (hwnd != nullptr)
            DestroyWindow(hwnd);

        if (registered)
            UnregisterClassW(otherCopyClassName, GetModuleHandleW(nullptr));
    }

    OtherCopyWindow(const OtherCopyWindow&) = delete;
    OtherCopyWindow& operator=(const OtherCopyWindow&) = delete;

    HWND hwnd = nullptr;
    bool registered = false;
};
} // namespace

auto tRunPendingForAnotherWindowIsDispatched =
    test("EventLoop/Windows/aRunPendingPostedToAnotherWindowReachesThatWindow") = []
{
    runPendingReceived = 0;

    auto otherCopy = OtherCopyWindow {};
    check(otherCopy.registered);
    check(otherCopy.hwnd != nullptr);

    auto ownCallbackRan = false;
    eacp::Threads::callAsync([&] { ownCallbackRan = true; });

    check(PostMessageW(otherCopy.hwnd, runPendingMessage, 0, 0) != 0);

    eacp::Threads::runEventLoopUntil([&] { return runPendingReceived > 0; },
                                     eacp::Time::MS {500});

    check(ownCallbackRan);
    check(runPendingReceived == 1);
};

auto tRepeatedRunForInstallsOneParentDeathWatchdog =
    test("EventLoop/Windows/repeatedRunForInstallsOneParentWatch") = []
{
    eacp::Threads::getEventLoop().runFor(eacp::Time::MS {0});
    auto threadsAfterFirstPass = countThreadsInThisProcess();
    auto handlesAfterFirstPass = countHandlesInThisProcess();

    for (auto pass = 0; pass < repeatedRunForPasses; ++pass)
        eacp::Threads::getEventLoop().runFor(eacp::Time::MS {0});

    check(countThreadsInThisProcess() == threadsAfterFirstPass);
    check(countHandlesInThisProcess() == handlesAfterFirstPass);
};

namespace
{
int runtimeThreadsLeftByLoopExitHarness(const std::string& mode)
{
    auto result = eacp::Processes::run(EACP_LOOP_EXIT_HARNESS, {mode});
    auto prefix = std::string {"runtime threads left: "};
    auto at = result.output.find(prefix);

    if (!result.exited || result.exitCode != 0 || at == std::string::npos)
        return -1;

    return std::stoi(result.output.substr(at + prefix.size()));
}
} // namespace

auto tRunLeavesNoRuntimeThreadBehind =
    test("EventLoop/Windows/aConsoleProgramsLoopLeavesNoRuntimeThreadBehind") = []
{ check(runtimeThreadsLeftByLoopExitHarness("run") == 0); };

auto tRunForLeavesNoRuntimeThreadBehind =
    test("EventLoop/Windows/aFirstRunForLeavesNoRuntimeThreadBehind") = []
{ check(runtimeThreadsLeftByLoopExitHarness("runFor") == 0); };

namespace
{
// LoopExitHarness's "wait" mode exits with this once its loop is quit.
constexpr auto harnessQuitExitCode = DWORD {7};

constexpr auto parentDeathQuitBudget = eacp::Time::MS {2000};

DWORD childIdIn(const std::string& output)
{
    auto prefix = std::string {"child "};
    auto at = output.find(prefix);
    auto end = output.find('\n', at);

    if (at == std::string::npos || end == std::string::npos)
        return 0;

    return (DWORD) std::stoul(output.substr(at + prefix.size()));
}

DWORD waitForChildId(eacp::Processes::Process& parent)
{
    auto deadline = eacp::Time::Deadline {eacp::Time::MS {10000}};

    while (!deadline.expired() && parent.isRunning())
    {
        if (auto id = childIdIn(parent.output()))
            return id;

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return childIdIn(parent.output());
}

struct HandleCounts
{
    int watching = 0;
    int detached = 0;
    int again = 0;
};

HandleCounts handlesAcrossDetachInLoopExitHarness()
{
    auto result = eacp::Processes::run(EACP_LOOP_EXIT_HARNESS, {"detach"});
    auto counts = HandleCounts {};

    sscanf_s(result.output.c_str(),
             "handles watching %d detached %d again %d",
             &counts.watching,
             &counts.detached,
             &counts.again);

    return counts;
}
} // namespace

auto tLoopQuitsWhenItsParentExits =
    test("EventLoop/Windows/theLoopQuitsWhenTheProcessThatStartedItExits") = []
{
    auto options = eacp::Processes::ProcessOptions {};
    options.executable = EACP_LOOP_EXIT_HARNESS;
    options.arguments = {"parent"};

    auto parent = eacp::Processes::Process {options};
    auto childId = waitForChildId(parent);
    check(childId != 0, parent.output());

    auto child = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION
                                 | PROCESS_TERMINATE,
                             FALSE,
                             childId);
    check(child != nullptr);

    parent.closeInput();
    check(parent.wait() == 0);

    if (child == nullptr)
        return;

    auto quit = WaitForSingleObject(child, (DWORD) parentDeathQuitBudget.count)
                == WAIT_OBJECT_0;

    if (!quit)
        TerminateProcess(child, 1);

    auto exitCode = DWORD {};
    GetExitCodeProcess(child, &exitCode);
    CloseHandle(child);

    check(quit);
    check(exitCode == harnessQuitExitCode);
};

auto tDetachReleasesTheParentWatch = test(
    "EventLoop/Windows/detachReleasesTheParentWatchAndALaterPassWatchesAgain") = []
{
    auto counts = handlesAcrossDetachInLoopExitHarness();

    check(counts.watching > 0);
    check(counts.detached < counts.watching);
    check(counts.again == counts.watching);
};
} // namespace EventLoopWindowsTests
