#include "Clipboard.h"

#include "../Utils/WinInclude.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstring>
#include <cwchar>
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

// UTF-16 -> UTF-8, the reverse of toWideString. Bounded by the caller's
// wcslen, so an unterminated handle cannot run away.
std::string toUtf8(const wchar_t* text, int length)
{
    if (text == nullptr || length <= 0)
        return {};

    auto required = WideCharToMultiByte(
        CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);

    if (required <= 0)
        return {};

    auto result = std::string(static_cast<std::size_t>(required), '\0');

    if (WideCharToMultiByte(CP_UTF8,
                            0,
                            text,
                            length,
                            result.data(),
                            required,
                            nullptr,
                            nullptr)
        != required)
        return {};

    return result;
}
} // namespace

std::string getText()
{
    // IsClipboardFormatAvailable first: opening the clipboard takes a global
    // lock that blocks every other application, so it is not worth taking when
    // there is no text to read.
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return {};

    if (!openClipboardWithRetry(nullptr))
        return {};

    auto result = std::string {};
    auto handle = GetClipboardData(CF_UNICODETEXT);

    if (handle != nullptr)
    {
        // The handle belongs to the clipboard, not to us: lock to read, unlock
        // when done, and never free it.
        if (const auto* data = static_cast<const wchar_t*>(GlobalLock(handle)))
        {
            result = toUtf8(data, static_cast<int>(std::wcslen(data)));
            GlobalUnlock(handle);
        }
    }

    CloseClipboard();
    return result;
}

bool hasText()
{
    return IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
}

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

    auto ok =
        EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
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

    auto* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(dropFiles)
                                              + sizeof(DROPFILES));
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
