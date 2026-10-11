#pragma once

#include <eacp/Core/Utils/WinInclude.h>
#include "FileDrag.h"

#include <ole2.h>
#include <wrl/client.h>

#include <string>

namespace eacp::Graphics
{

Microsoft::WRL::ComPtr<IDataObject>
    createFileDataObject(const Vector<std::string>& paths);

FileDragResult fileDragResultFor(HRESULT dragResult, DWORD effect);

} // namespace eacp::Graphics
