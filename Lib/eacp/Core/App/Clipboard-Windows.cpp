#include "Clipboard.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <shellapi.h>
#include <shlobj.h>

#include <cstring>
#include <limits>
#include <vector>

namespace eacp::Clipboard
{
namespace
{
bool toWideString(std::string_view text, std::wstring& result)
{
    result.clear();

    if (text.empty())
        return true;

    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;

    auto required = MultiByteToWideChar(CP_UTF8,
                                        MB_ERR_INVALID_CHARS,
                                        text.data(),
                                        static_cast<int>(text.size()),
                                        nullptr,
                                        0);
    if (required <= 0)
        return false;

    result.assign(static_cast<std::size_t>(required), L'\0');
    return MultiByteToWideChar(CP_UTF8,
                               MB_ERR_INVALID_CHARS,
                               text.data(),
                               static_cast<int>(text.size()),
                               result.data(),
                               required)
           == required;
}

bool openClipboardWithRetry(HWND owner)
{
    for (auto attempt = 0; attempt < 5; ++attempt)
    {
        if (OpenClipboard(owner))
            return true;

        Sleep(10);
    }

    return false;
}
} // namespace

bool copyText(std::string_view text)
{
    auto wide = std::wstring {};
    if (!toWideString(text, wide))
        return false;

    auto bytes = (wide.size() + 1) * sizeof(wchar_t);
    auto handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle)
        return false;

    auto* data = static_cast<wchar_t*>(GlobalLock(handle));
    if (!data)
    {
        GlobalFree(handle);
        return false;
    }

    std::memcpy(data, wide.c_str(), bytes);
    GlobalUnlock(handle);

    if (!openClipboardWithRetry(nullptr))
    {
        GlobalFree(handle);
        return false;
    }

    auto ok = EmptyClipboard()
              && SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
    CloseClipboard();

    if (!ok)
        GlobalFree(handle);

    return ok;
}

bool copyFiles(const Vector<std::string>& paths)
{
    if (paths.empty())
        return false;

    auto widePaths = std::vector<std::wstring> {};
    auto pathChars = std::size_t {1};

    for (const auto& path: paths)
    {
        auto wide = std::wstring {};
        if (!toWideString(path, wide))
            return false;

        if (wide.empty())
            continue;

        pathChars += wide.size() + 1;
        widePaths.push_back(std::move(wide));
    }

    if (widePaths.empty())
        return false;

    auto bytes = sizeof(DROPFILES) + pathChars * sizeof(wchar_t);
    auto handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!handle)
        return false;

    auto* dropFiles = static_cast<DROPFILES*>(GlobalLock(handle));
    if (!dropFiles)
    {
        GlobalFree(handle);
        return false;
    }

    dropFiles->pFiles = sizeof(DROPFILES);
    dropFiles->fWide = TRUE;

    auto* cursor = reinterpret_cast<wchar_t*>(
        reinterpret_cast<char*>(dropFiles) + sizeof(DROPFILES));
    for (const auto& path: widePaths)
    {
        std::memcpy(cursor, path.c_str(), path.size() * sizeof(wchar_t));
        cursor += path.size() + 1;
    }

    GlobalUnlock(handle);

    if (!openClipboardWithRetry(nullptr))
    {
        GlobalFree(handle);
        return false;
    }

    auto ok = EmptyClipboard() && SetClipboardData(CF_HDROP, handle) != nullptr;
    CloseClipboard();

    if (!ok)
        GlobalFree(handle);

    return ok;
}
} // namespace eacp::Clipboard
