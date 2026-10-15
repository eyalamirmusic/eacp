#include "WinInclude.h"

#include "FilesPlatform.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <string>

namespace eacp::Detail
{
namespace
{
// The shell wants COM on the calling thread: SHOpenFolderAndSelectItems
// requires it, and ShellExecuteW asks for it because the handler it runs may
// use it. An app thread usually has it already, as an STA (RPC_E_CHANGED_MODE
// when it is an MTA, which still works); either way this one is undone only
// if it was the call that set it up.
struct ScopedCom
{
    ScopedCom()
        : result(CoInitializeEx(nullptr,
                                COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))
    {
    }

    ~ScopedCom()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }

    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;

    HRESULT result;
};

// FilePath keeps '/', which ShellExecuteW tolerates but the shell's item
// parsing and explorer's /select switch do not.
std::wstring nativePath(const FilePath& target)
{
    auto wide = target.wide();
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    return wide;
}

// ShellExecuteW answers an HINSTANCE that is really an error code, success
// being anything above 32.
bool shellExecute(const wchar_t* file, const wchar_t* parameters)
{
    auto result =
        ShellExecuteW(nullptr, nullptr, file, parameters, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool selectInExplorer(const std::wstring& path)
{
    auto* item = ILCreateFromPathW(path.c_str());

    if (item == nullptr)
        return false;

    auto result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    ILFree(item);

    return SUCCEEDED(result);
}
} // namespace

// The default verb rather than "open": it is what a double-click runs, and
// for a type whose default is something else "open" may not be registered at
// all. A directory's default verb opens it in Explorer.
bool openWithSystem(const FilePath& target)
{
    auto com = ScopedCom {};
    auto path = nativePath(target);

    return shellExecute(path.c_str(), nullptr);
}

bool revealInFileManager(const FilePath& target)
{
    auto com = ScopedCom {};
    auto path = nativePath(target);

    if (selectInExplorer(path))
        return true;

    auto parameters = L"/select,\"" + path + L"\"";
    return shellExecute(L"explorer.exe", parameters.c_str());
}
} // namespace eacp::Detail
