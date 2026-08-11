#include "../Utils/WinInclude.h"

#include "App.h"
#include "App-Windows-FilePicker.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <winrt/base.h>

namespace eacp::Apps
{
// No Dock concept on Windows.
void setDockIconVisible(bool) {}

// Presence-only check for an embedded Authenticode signature, deliberately not
// WinVerifyTrust: its chain validation depends on expiry and the network.
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
std::optional<std::wstring> showShellOpenDialog(bool pickFolders,
                                                const FilePickerOptions& options)
{
    const auto coInit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninitialize = SUCCEEDED(coInit);

    // The dialog lives in this lambda so its com_ptrs release before the
    // CoUninitialize below.
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
std::optional<std::wstring> showShellSaveDialog(const FileSaveOptions& options)
{
    const auto coInit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninitialize = SUCCEEDED(coInit);

    // The dialog lives in this lambda so its com_ptrs release before the
    // CoUninitialize below.
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

            const auto defaultExtension =
                std::wstring(winrt::to_hstring(options.allowedExtensions[0]));
            dialog->SetDefaultExtension(defaultExtension.c_str());
        }

        if (!options.suggestedName.empty())
        {
            const auto name = std::wstring(winrt::to_hstring(options.suggestedName));
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
