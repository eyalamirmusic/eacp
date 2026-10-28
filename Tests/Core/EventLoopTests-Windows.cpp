// A plug-in DLL linking its own eacp copy posts WM_EACP_RUN_PENDING to its own
// message-only window. An eacp host pumping the thread must dispatch it there
// rather than take it for its own. A window of this file stands in for the
// other copy's.

#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>

using namespace nano;

namespace EventLoopWindowsTests
{
namespace
{
// WM_EACP_RUN_PENDING, file-local in EventLoop-Windows.cpp. Keep the two in step.
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
} // namespace EventLoopWindowsTests
