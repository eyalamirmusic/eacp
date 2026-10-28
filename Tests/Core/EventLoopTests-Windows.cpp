// Another eacp copy's wake-up, pumped by this copy's loop.
//
// The case it stands for: a plug-in DLL that statically links its own eacp
// copy, hosted by an executable that links another. The DLL's callAsync posts
// WM_EACP_RUN_PENDING to the DLL copy's own message-only window
// (EventLoop::call in EventLoop-Windows.cpp), counting on whoever pumps the
// thread to DispatchMessage it into that copy's window procedure. A DAW's pump
// does. An eacp host pumping with runEventLoopFor / runEventLoopUntil does not:
// EventLoop::runFor (and EventLoop::run) catch WM_EACP_RUN_PENDING by message id
// alone, drain the HOST copy's queue and move on without dispatching, so the
// DLL's callback never runs.
//
// One process is enough to show it: a message-only window of this file's own
// class stands in for the other copy's window, and the message is posted to it
// exactly as that copy's EventLoop::call would.
//
// EXPECTED TO FAIL until the loop handles WM_EACP_RUN_PENDING inline only when
// msg.hwnd is null (a thread message) and dispatches every message addressed to
// a window, its own message window included.

#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>

using namespace nano;

namespace EventLoopWindowsTests
{
namespace
{
// EventLoop-Windows.cpp's WM_EACP_RUN_PENDING, which is file-local there, so it
// is restated here. Every eacp copy computes the same id, which is the whole
// problem: the message looks like this copy's own whichever window it is for.
// Keep the two in step.
constexpr auto runPendingMessage = UINT {WM_APP + 0x42E1};

constexpr auto otherCopyClassName = L"EACPEventLoopTestOtherCopyWindow";

auto runPendingReceived = 0;

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

// The other copy's message-only window, registered and created as
// ensureMessageWindow does it, and taken down again with the test.
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

    // This copy's own queue, to show the loop did pump while the other copy's
    // message went missing.
    auto ownCallbackRan = false;
    eacp::Threads::callAsync([&] { ownCallbackRan = true; });

    check(PostMessageW(otherCopy.hwnd, runPendingMessage, 0, 0) != 0);

    eacp::Threads::runEventLoopUntil([&] { return runPendingReceived > 0; },
                                     eacp::Time::MS {500});

    check(ownCallbackRan);
    check(runPendingReceived == 1);
};
} // namespace EventLoopWindowsTests
