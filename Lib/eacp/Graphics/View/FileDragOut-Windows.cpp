#include "FileDragOut-Windows.h"

#include <eacp/Core/Utils/Strings.h>

#include <shlobj.h>

#include <algorithm>

namespace eacp::Graphics
{

Microsoft::WRL::ComPtr<IDataObject>
    createFileDataObject(const Vector<std::string>& paths)
{
    auto pidls = Vector<PIDLIST_ABSOLUTE> {};

    for (auto& path: paths)
    {
        auto widePath = Strings::widen(path);
        std::replace(widePath.begin(), widePath.end(), L'/', L'\\');

        PIDLIST_ABSOLUTE pidl = nullptr;

        if (SUCCEEDED(SHParseDisplayName(widePath.c_str(), nullptr, &pidl, 0, nullptr))
            && pidl != nullptr)
            pidls.add(pidl);
    }

    auto dataObject = Microsoft::WRL::ComPtr<IDataObject> {};

    if (!pidls.empty())
    {
        auto items = Microsoft::WRL::ComPtr<IShellItemArray> {};

        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(
                static_cast<UINT>(pidls.size()),
                const_cast<LPCITEMIDLIST*>(pidls.data()),
                &items))
            && items)
            items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dataObject));
    }

    for (auto* pidl: pidls)
        CoTaskMemFree(pidl);

    return dataObject;
}

FileDragResult fileDragResultFor(HRESULT dragResult, DWORD effect)
{
    if (dragResult != DRAGDROP_S_DROP)
        return FileDragResult::Cancelled;

    if ((effect & DROPEFFECT_MOVE) != 0)
        return FileDragResult::Moved;

    return FileDragResult::Copied;
}

} // namespace eacp::Graphics
