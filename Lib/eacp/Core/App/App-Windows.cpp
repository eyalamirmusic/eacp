#include "../Utils/WinInclude.h"

#include "App.h"
#include "App-Windows-FilePicker.h"

#include "../Utils/Strings.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <winrt/base.h>

namespace eacp::Apps
{
// No Dock concept on Windows; an app with no Window already has no taskbar
// button, so there is nothing to toggle.
void setDockIconVisible(bool) {}

namespace
{
// The overlay belongs to a window rather than to the process, so the badge
// needs one: the first visible, unowned top-level window this process has.
HWND findMainWindow()
{
    struct Search
    {
        DWORD processId = GetCurrentProcessId();
        HWND found = nullptr;
    };

    auto search = Search {};

    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL
        {
            auto& state = *reinterpret_cast<Search*>(param);

            auto owner = DWORD {};
            GetWindowThreadProcessId(window, &owner);

            if (owner != state.processId || !IsWindowVisible(window)
                || GetWindow(window, GW_OWNER) != nullptr)
                return TRUE;

            state.found = window;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));

    return search.found;
}

// The taskbar takes an icon, not a string, so the text is drawn into one at
// the system's small-icon size. GDI's text output leaves the alpha channel
// alone, which on a 32-bit DIB means "transparent" — so the disc is filled
// into the pixels directly and every pixel inside it is forced opaque again
// once DrawText has run, rather than trusting the alpha GDI left behind.
HICON createBadgeIcon(const std::string& text)
{
    const auto size = GetSystemMetrics(SM_CXSMICON);
    const auto radius = (float) size * 0.5f;

    auto isInsideDisc = [radius](int x, int y)
    {
        const auto dx = (float) x + 0.5f - radius;
        const auto dy = (float) y + 0.5f - radius;
        return dx * dx + dy * dy <= radius * radius;
    };

    auto info = BITMAPINFO {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    auto color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);

    if (color == nullptr)
        return nullptr;

    auto* pixels = static_cast<std::uint32_t*>(bits);

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            pixels[y * size + x] = isInsideDisc(x, y) ? 0xffed3b3bu : 0u;

    auto screen = GetDC(nullptr);
    auto dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    auto previousBitmap = SelectObject(dc, color);

    auto font = CreateFontW(-(size * 3) / 5,
                            0,
                            0,
                            0,
                            FW_BOLD,
                            FALSE,
                            FALSE,
                            FALSE,
                            DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE,
                            L"Segoe UI");

    auto previousFont = SelectObject(dc, font);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    auto wide = Strings::widen(text);
    auto textBounds = RECT {0, 0, size, size};

    DrawTextW(dc,
              wide.c_str(),
              (int) wide.size(),
              &textBounds,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

    GdiFlush();

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto& pixel = pixels[y * size + x];
            pixel = isInsideDisc(x, y) ? pixel | 0xff000000u : 0u;
        }

    SelectObject(dc, previousFont);
    DeleteObject(font);
    SelectObject(dc, previousBitmap);
    DeleteDC(dc);

    // 1-bit scanlines are WORD aligned. The mask goes unused for a 32-bit
    // icon whose alpha carries the shape, but CreateIconIndirect still wants
    // one, and one full of whatever CreateBitmap left behind is not it.
    auto maskBits = Vector<std::uint8_t>(((size + 15) / 16) * 2 * size);
    auto mask = CreateBitmap(size, size, 1, 1, maskBits.data());

    auto iconInfo = ICONINFO {};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = color;
    iconInfo.hbmMask = mask;

    auto icon = CreateIconIndirect(&iconInfo);

    DeleteObject(mask);
    DeleteObject(color);

    return icon;
}
} // namespace

void setAppBadge(const std::string& text)
{
    auto window = findMainWindow();

    if (window == nullptr)
        return;

    const auto coInit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninitialize = SUCCEEDED(coInit);

    // COM objects must release before CoUninitialize, so the taskbar list
    // lives in this scope — the same shape the file pickers above use.
    {
        auto taskbar = winrt::com_ptr<ITaskbarList3> {};

        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList,
                                       nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(taskbar.put())))
            && SUCCEEDED(taskbar->HrInit()))
        {
            if (text.empty())
            {
                taskbar->SetOverlayIcon(window, nullptr, nullptr);
            }
            else
            {
                auto icon = createBadgeIcon(text);
                auto description = Strings::widen(text);

                taskbar->SetOverlayIcon(window, icon, description.c_str());

                // The shell keeps its own copy, so this one goes straight back.
                if (icon != nullptr)
                    DestroyIcon(icon);
            }
        }
    }

    if (shouldUninitialize)
        CoUninitialize();
}

// No OS-level power-off announcement to watch for, so a quit request is
// never the system's (see App.h).
bool isSystemPoweringOff()
{
    return false;
}

void Detail::observeSystemPowerOff() {}

// Presence-only check for an embedded Authenticode signature — deliberately
// not WinVerifyTrust, whose full chain validation is sensitive to expiry,
// revocation and network availability (see App.h).
bool isDistributionSigned()
{
    wchar_t path[MAX_PATH] = {};

    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
        return false;

    auto contentType = DWORD {};

    return CryptQueryObject(CERT_QUERY_OBJECT_FILE,
                            path,
                            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                            CERT_QUERY_FORMAT_FLAG_BINARY,
                            0,
                            nullptr,
                            &contentType,
                            nullptr,
                            nullptr,
                            nullptr,
                            nullptr)
           != FALSE;
}

void openExternalURL(const std::string& url)
{
    if (url.empty())
        return;

    auto wide = Strings::widen(url);

    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

namespace detail
{
std::wstring buildFilterPattern(const Vector<std::string>& extensions)
{
    auto pattern = std::wstring {};
    for (const auto& extension: extensions)
    {
        if (!pattern.empty())
            pattern += L';';

        pattern += L"*.";
        // Extensions are ASCII, so a per-char widen is exact.
        for (char c: extension)
            pattern += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    return pattern;
}

std::optional<std::string> shellResultToPath(const wchar_t* pickedWidePath)
{
    if (pickedWidePath == nullptr || pickedWidePath[0] == L'\0')
        return std::nullopt;

    auto out = Strings::narrow(pickedWidePath);
    if (out.empty())
        return std::nullopt;

    return out;
}

std::optional<std::string> chooseWithDialog(bool pickFolders,
                                            const FilePickerOptions& options,
                                            const ShellOpenDialog& dialog)
{
    const auto picked = dialog(pickFolders, options);
    if (!picked)
        return std::nullopt;

    return shellResultToPath(picked->c_str());
}

std::optional<std::string> chooseSaveWithDialog(const FileSaveOptions& options,
                                                const ShellSaveDialog& dialog)
{
    const auto picked = dialog(options);
    if (!picked)
        return std::nullopt;

    return shellResultToPath(picked->c_str());
}
} // namespace detail

namespace
{
// Real IFileOpenDialog behind the detail:: seam; tests substitute a fake.
std::optional<std::wstring> showShellOpenDialog(bool pickFolders,
                                                const FilePickerOptions& options)
{
    const auto coInit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninitialize = SUCCEEDED(coInit);

    // COM objects must release before CoUninitialize, so the dialog lives in
    // this lambda — its com_ptrs unwind before the uninit below.
    const auto result = [&]() -> std::optional<std::wstring>
    {
        auto dialog = winrt::com_ptr<IFileOpenDialog> {};
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,
                                    nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(dialog.put()))))
            return std::nullopt;

        auto flags = FILEOPENDIALOGOPTIONS {};
        if (SUCCEEDED(dialog->GetOptions(&flags)))
        {
            flags = static_cast<FILEOPENDIALOGOPTIONS>(
                flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST
                | (pickFolders ? FOS_PICKFOLDERS : 0));
            dialog->SetOptions(flags);
        }

        if (!pickFolders && !options.allowedExtensions.empty())
        {
            const auto pattern =
                detail::buildFilterPattern(options.allowedExtensions);
            const COMDLG_FILTERSPEC specs[] = {
                {L"Supported files", pattern.c_str()},
                {L"All files", L"*.*"},
            };
            dialog->SetFileTypes(2, specs);
            dialog->SetFileTypeIndex(1);
        }

        if (FAILED(dialog->Show(nullptr)))
            return std::nullopt;

        auto item = winrt::com_ptr<IShellItem> {};
        if (FAILED(dialog->GetResult(item.put())) || !item)
            return std::nullopt;

        PWSTR rawPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))
            || rawPath == nullptr)
            return std::nullopt;

        auto path = std::wstring {rawPath};
        CoTaskMemFree(rawPath);
        return path;
    }();

    if (shouldUninitialize)
        CoUninitialize();

    return result;
}
// Real IFileSaveDialog behind the detail:: seam; tests substitute a fake.
// FOS_OVERWRITEPROMPT is the dialog's own confirmation — the caller writes the
// returned path unconditionally.
std::optional<std::wstring> showShellSaveDialog(const FileSaveOptions& options)
{
    const auto coInit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninitialize = SUCCEEDED(coInit);

    // COM objects must release before CoUninitialize, so the dialog lives in
    // this lambda — its com_ptrs unwind before the uninit below.
    const auto result = [&]() -> std::optional<std::wstring>
    {
        auto dialog = winrt::com_ptr<IFileSaveDialog> {};
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog,
                                    nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(dialog.put()))))
            return std::nullopt;

        auto flags = FILEOPENDIALOGOPTIONS {};
        if (SUCCEEDED(dialog->GetOptions(&flags)))
        {
            flags = static_cast<FILEOPENDIALOGOPTIONS>(flags | FOS_FORCEFILESYSTEM
                                                       | FOS_PATHMUSTEXIST
                                                       | FOS_OVERWRITEPROMPT);
            dialog->SetOptions(flags);
        }

        if (!options.allowedExtensions.empty())
        {
            const auto pattern =
                detail::buildFilterPattern(options.allowedExtensions);
            const COMDLG_FILTERSPEC specs[] = {
                {L"Supported files", pattern.c_str()},
                {L"All files", L"*.*"},
            };
            dialog->SetFileTypes(2, specs);
            dialog->SetFileTypeIndex(1);

            // So a name typed without one still lands on the right extension.
            const auto defaultExtension =
                Strings::widen(options.allowedExtensions[0]);
            dialog->SetDefaultExtension(defaultExtension.c_str());
        }

        if (!options.suggestedName.empty())
        {
            const auto name = Strings::widen(options.suggestedName);
            dialog->SetFileName(name.c_str());
        }

        if (FAILED(dialog->Show(nullptr)))
            return std::nullopt;

        auto item = winrt::com_ptr<IShellItem> {};
        if (FAILED(dialog->GetResult(item.put())) || !item)
            return std::nullopt;

        PWSTR rawPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))
            || rawPath == nullptr)
            return std::nullopt;

        auto path = std::wstring {rawPath};
        CoTaskMemFree(rawPath);
        return path;
    }();

    if (shouldUninitialize)
        CoUninitialize();

    return result;
}
} // namespace

std::optional<std::string> chooseFile(const FilePickerOptions& options)
{
    return detail::chooseWithDialog(false, options, showShellOpenDialog);
}

std::optional<std::string> chooseSaveFile(const FileSaveOptions& options)
{
    return detail::chooseSaveWithDialog(options, showShellSaveDialog);
}

std::optional<std::string> chooseDirectory()
{
    return detail::chooseWithDialog(true, {}, showShellOpenDialog);
}
} // namespace eacp::Apps
