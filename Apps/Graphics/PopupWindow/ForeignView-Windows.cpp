#include "ForeignView.h"

#include <eacp/Core/Utils/WinInclude.h>

// Two stock Win32 controls in the host's child window. A BUTTON notifies its
// parent with WM_COMMAND, and the parent here is eacp's own child window, so
// the press is not printed the way the macOS one is — what this shows is that
// the popup covers a real foreign HWND and that the press never reaches it.
void addForeignContent(void* nativeParentHandle)
{
    auto parent = (HWND) nativeParentHandle;

    if (parent == nullptr)
        return;

    CreateWindowExW(0,
                    L"STATIC",
                    L"Foreign native window - a plugin editor, not an eacp View",
                    WS_CHILD | WS_VISIBLE,
                    16,
                    16,
                    440,
                    24,
                    parent,
                    nullptr,
                    nullptr,
                    nullptr);

    CreateWindowExW(0,
                    L"BUTTON",
                    L"Plugin button",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    16,
                    56,
                    160,
                    32,
                    parent,
                    nullptr,
                    nullptr,
                    nullptr);
}
