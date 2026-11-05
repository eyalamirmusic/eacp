#include "Alert.h"

#include "../Platform/Platform.h"
#include "../Utils/Strings.h"
#include "../Utils/WinInclude.h"

#include <commctrl.h>

#include <optional>
#include <string>

namespace eacp::Apps
{
static_assert(Detail::WindowsIds::ok == IDOK);
static_assert(Detail::WindowsIds::cancel == IDCANCEL);
static_assert(Detail::WindowsIds::yes == IDYES);
static_assert(Detail::WindowsIds::no == IDNO);

namespace
{
using TaskDialogIndirectFunction = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*,
                                                    int*,
                                                    int*,
                                                    BOOL*);

// TaskDialogIndirect is comctl32 v6's alone, and an executable gets v5 unless
// its manifest asks for v6. The manifest shell32 carries as resource 124 does,
// so activating it around the dialog gives v6 whatever the app's says, without
// changing the process default.
//
// The context stays active for the whole of the dialog's modal loop, so every
// eacp callback, timer and window procedure that runs inside it runs with
// shell32's manifest active too: a LoadLibrary, a common control's class or a
// side-by-side COM activation there resolves against v6. Nested alerts
// activate and deactivate in LIFO order, as the API requires.
struct CommonControlsV6Scope
{
    CommonControlsV6Scope()
    {
        wchar_t systemDirectory[MAX_PATH] {};

        if (GetSystemDirectoryW(systemDirectory, MAX_PATH) == 0)
            return;

        auto request = ACTCTXW {};
        request.cbSize = sizeof(request);
        request.dwFlags =
            ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
        request.lpSource = L"shell32.dll";
        request.lpAssemblyDirectory = systemDirectory;
        request.lpResourceName = MAKEINTRESOURCEW(124);

        context = CreateActCtxW(&request);

        if (context != INVALID_HANDLE_VALUE)
            activated = ActivateActCtx(context, &cookie) != FALSE;

        comctl32 = LoadLibraryW(L"comctl32.dll");
    }

    ~CommonControlsV6Scope()
    {
        if (comctl32 != nullptr)
            FreeLibrary(comctl32);

        if (activated)
            DeactivateActCtx(0, cookie);

        if (context != INVALID_HANDLE_VALUE)
            ReleaseActCtx(context);
    }

    CommonControlsV6Scope(const CommonControlsV6Scope&) = delete;
    CommonControlsV6Scope& operator=(const CommonControlsV6Scope&) = delete;

    TaskDialogIndirectFunction taskDialogIndirect() const
    {
        if (comctl32 == nullptr)
            return nullptr;

        return procAddress<TaskDialogIndirectFunction>(comctl32,
                                                       "TaskDialogIndirect");
    }

    HANDLE context = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie = 0;
    bool activated = false;
    HMODULE comctl32 = nullptr;
};

BOOL CALLBACK addEnabledVisibleWindow(HWND window, LPARAM windows)
{
    if (IsWindowVisible(window) && IsWindowEnabled(window))
        reinterpret_cast<Vector<HWND>*>(windows)->add(window);

    return TRUE;
}

// The calling thread's visible, enabled top-level windows, topmost first.
Vector<HWND> threadWindows()
{
    auto windows = Vector<HWND> {};
    EnumThreadWindows(GetCurrentThreadId(),
                      addEnabledVisibleWindow,
                      reinterpret_cast<LPARAM>(&windows));
    return windows;
}

// The active window, else the thread's topmost one, so an alert raised while
// the app is in the background still has a window to come up over.
HWND alertOwner()
{
    if (auto active = GetActiveWindow())
        return active;

    auto windows = threadWindows();
    return windows.empty() ? nullptr : windows[0];
}

// A dialog disables only its owner. The thread's other top-level windows are
// disabled here for as long as it is up, so the whole app waits on it.
struct OtherWindowsDisabled
{
    explicit OtherWindowsDisabled(HWND owner)
    {
        for (auto window: threadWindows())
        {
            if (window != owner)
            {
                EnableWindow(window, FALSE);
                disabled.add(window);
            }
        }
    }

    ~OtherWindowsDisabled()
    {
        for (auto window: disabled)
            if (IsWindow(window))
                EnableWindow(window, TRUE);
    }

    OtherWindowsDisabled(const OtherWindowsDisabled&) = delete;
    OtherWindowsDisabled& operator=(const OtherWindowsDisabled&) = delete;

    Vector<HWND> disabled;
};

PCWSTR taskDialogIconFor(AlertIcon icon)
{
    switch (icon)
    {
        case AlertIcon::warning:
            return TD_WARNING_ICON;
        case AlertIcon::error:
            return TD_ERROR_ICON;
        case AlertIcon::info:
            break;
    }

    return TD_INFORMATION_ICON;
}

UINT messageBoxIconFor(AlertIcon icon)
{
    switch (icon)
    {
        case AlertIcon::warning:
            return MB_ICONWARNING;
        case AlertIcon::error:
            return MB_ICONERROR;
        case AlertIcon::info:
            break;
    }

    return MB_ICONINFORMATION;
}

UINT messageBoxButtonsFor(Detail::MessageBoxButtons buttons)
{
    switch (buttons)
    {
        case Detail::MessageBoxButtons::okCancel:
            return MB_OKCANCEL;
        case Detail::MessageBoxButtons::yesNo:
            return MB_YESNO;
        case Detail::MessageBoxButtons::yesNoCancel:
            return MB_YESNOCANCEL;
        case Detail::MessageBoxButtons::ok:
            break;
    }

    return MB_OK;
}

// std::nullopt only when TaskDialogIndirect cannot be found. A dialog that ran
// and failed is answered as unanswered rather than followed by a message box,
// since the user may already have seen it.
std::optional<int> showTaskDialog(HWND owner, const AlertOptions& options)
{
    auto scope = CommonControlsV6Scope {};
    auto taskDialogIndirect = scope.taskDialogIndirect();

    if (taskDialogIndirect == nullptr)
        return std::nullopt;

    auto labels = Vector<std::wstring> {};

    for (auto& button: options.buttons)
        labels.add(Strings::widen(button));

    auto buttons = Vector<TASKDIALOG_BUTTON> {};

    for (auto index = 0; index < labels.size(); ++index)
        buttons.add(
            TASKDIALOG_BUTTON {Detail::WindowsIds::firstTaskDialogButton + index,
                               labels[index].c_str()});

    auto windowTitle = Strings::widen(Platform::getAppName());
    auto title = Strings::widen(options.title);
    auto message = Strings::widen(options.message);

    auto config = TASKDIALOGCONFIG {};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.pszWindowTitle = windowTitle.empty() ? nullptr : windowTitle.c_str();
    config.pszMainIcon = taskDialogIconFor(options.icon);
    config.pszMainInstruction = title.c_str();
    config.pszContent = message.c_str();
    config.cButtons = (UINT) buttons.size();
    config.pButtons = buttons.data();
    config.nDefaultButton =
        Detail::WindowsIds::firstTaskDialogButton + options.defaultButton;

    if (owner != nullptr)
        config.dwFlags |= TDF_POSITION_RELATIVE_TO_WINDOW;

    if (options.cancelButton)
        config.dwFlags |= TDF_ALLOW_DIALOG_CANCELLATION;

    auto pressed = 0;
    auto result = taskDialogIndirect(&config, &pressed, nullptr, nullptr);

    if (FAILED(result))
    {
        LOGF("Alert: TaskDialogIndirect failed (hr = 0x{:08X}), unanswered",
             (unsigned long) result);
        return Detail::unansweredButton(options);
    }

    return Detail::answerForTaskDialog(options, pressed);
}

// MessageBoxW has fixed buttons; Detail::planMessageBox maps the alert's onto
// them, and the message names the real labels where they differ.
int showMessageBox(HWND owner, const AlertOptions& options)
{
    LOG("Alert: TaskDialogIndirect is unavailable, showing a message box");

    auto plan = Detail::planMessageBox(options);
    auto title = Strings::widen(options.title);
    auto message = Strings::widen(Detail::messageBoxText(options, plan));

    auto defaultButton = (UINT) plan.defaultBoxButton * (UINT) MB_DEFBUTTON2;
    auto flags = messageBoxButtonsFor(plan.buttons) | messageBoxIconFor(options.icon)
                 | defaultButton;

    if (owner == nullptr)
        flags |= (UINT) MB_TASKMODAL;

    auto id = MessageBoxW(owner, message.c_str(), title.c_str(), flags);

    return Detail::answerForMessageBox(options, plan, id);
}
} // namespace

// Modal over the whole app from the event loop, as JUCE's showMessageBoxAsync
// is: owned by the active window (or the thread's topmost one), with the
// thread's other top-level windows disabled until it is answered.
void Detail::showNativeAlert(const AlertOptions& options, AlertCallback onDismissed)
{
    auto owner = alertOwner();
    auto othersDisabled = OtherWindowsDisabled {owner};

    if (auto pressed = showTaskDialog(owner, options))
        onDismissed(*pressed);
    else
        onDismissed(showMessageBox(owner, options));
}
} // namespace eacp::Apps
