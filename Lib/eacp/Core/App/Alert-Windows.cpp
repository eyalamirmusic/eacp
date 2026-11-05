#include "Alert.h"

#include "../Platform/Platform.h"
#include "../Utils/Strings.h"
#include "../Utils/WinInclude.h"

#include <commctrl.h>

#include <algorithm>
#include <optional>
#include <string>

namespace eacp::Apps
{
namespace
{
constexpr auto firstAlertButtonId = 100;

using TaskDialogIndirectFunction = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*,
                                                    int*,
                                                    int*,
                                                    BOOL*);

// TaskDialogIndirect is comctl32 v6's alone, and an executable gets v5 unless
// its manifest asks for v6. The manifest shell32 carries as resource 124 does,
// so activating it around the dialog gives v6 whatever the app's says, without
// changing the process default.
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

int pressedButton(int id, const AlertOptions& options)
{
    if (id == IDCANCEL && options.cancelButton)
        return *options.cancelButton;

    auto index = id - firstAlertButtonId;

    if (index >= 0 && index < options.buttons.size())
        return index;

    return options.defaultButton;
}

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
            TASKDIALOG_BUTTON {firstAlertButtonId + index, labels[index].c_str()});

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
    config.nDefaultButton = firstAlertButtonId + options.defaultButton;

    if (owner != nullptr)
        config.dwFlags |= TDF_POSITION_RELATIVE_TO_WINDOW;

    if (options.cancelButton)
        config.dwFlags |= TDF_ALLOW_DIALOG_CANCELLATION;

    auto pressed = 0;

    if (FAILED(taskDialogIndirect(&config, &pressed, nullptr, nullptr)))
        return std::nullopt;

    return pressedButton(pressed, options);
}

// MessageBoxW has fixed buttons, so their labels are lost: one button is OK,
// two are OK and Cancel, three or more Yes, No and Cancel, and nothing past the
// third can be pressed.
int showMessageBox(HWND owner, const AlertOptions& options)
{
    auto count = options.buttons.size();
    auto layout = count == 1 ? MB_OK : count == 2 ? MB_OKCANCEL : MB_YESNOCANCEL;
    auto defaultButton = (UINT) std::min(options.defaultButton, 2) * MB_DEFBUTTON2;

    auto title = Strings::widen(options.title);
    auto message = Strings::widen(options.message);

    auto id =
        MessageBoxW(owner,
                    message.c_str(),
                    title.c_str(),
                    (UINT) layout | defaultButton | messageBoxIconFor(options.icon));

    switch (id)
    {
        case IDOK:
        case IDYES:
            return 0;
        case IDNO:
            return 1;
        case IDCANCEL:
            return count == 1 ? 0 : count == 2 ? 1 : 2;
        default:
            return options.defaultButton;
    }
}
} // namespace

// App-modal from the event loop, owned by the calling thread's active window
// when it has one, as JUCE's showMessageBoxAsync is.
void Detail::showNativeAlert(const AlertOptions& options, AlertCallback onDismissed)
{
    auto owner = GetActiveWindow();

    if (auto pressed = showTaskDialog(owner, options))
        onDismissed(*pressed);
    else
        onDismissed(showMessageBox(owner, options));
}
} // namespace eacp::Apps
