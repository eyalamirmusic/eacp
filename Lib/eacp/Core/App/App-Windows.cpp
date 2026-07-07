#include "../Utils/WinInclude.h"

#include "App.h"
#include "App-Windows-FilePicker.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <winrt/base.h>

#include <optional>
#include <string>

namespace eacp::Apps
{
// No Dock concept on Windows; an app with no Window already has no taskbar
// button, so there is nothing to toggle.
void setDockIconVisible(bool) {}

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

    auto wide = std::wstring(winrt::to_hstring(url));

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

    const auto size = WideCharToMultiByte(
        CP_UTF8, 0, pickedWidePath, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return std::nullopt;

    auto out = std::string(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, pickedWidePath, -1, out.data(), size, nullptr, nullptr);
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
} // namespace

std::optional<std::string> chooseFile(const FilePickerOptions& options)
{
    return detail::chooseWithDialog(false, options, showShellOpenDialog);
}

std::optional<std::string> chooseDirectory()
{
    return detail::chooseWithDialog(true, {}, showShellOpenDialog);
}
} // namespace eacp::Apps
