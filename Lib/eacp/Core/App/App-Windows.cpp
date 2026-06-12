#include "../Utils/WinInclude.h"

#include "App.h"

#include <shellapi.h>
#include <wincrypt.h>
#include <winrt/base.h>

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

// TODO: implement with IFileOpenDialog.
std::optional<std::string> chooseFile(const FilePickerOptions&)
{
    return std::nullopt;
}

// TODO: implement with IFileOpenDialog (FOS_PICKFOLDERS).
std::optional<std::string> chooseDirectory()
{
    return std::nullopt;
}
} // namespace eacp::Apps
