#pragma once

#include "../Device/AdapterDescription.h"

#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/WinInclude.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <winrt/base.h>

#include <string_view>

namespace eacp::GPU
{
winrt::com_ptr<IDXGIAdapter1> findAdapterOf(ID3D12Device* device);

// Before the driver check, so a driver that crashes inside it has already been
// named in the log.
void logEveryAdapter();

AdapterDescription describeDeviceAdapter(ID3D12Device* device);

// A line before and after, so a driver that crashes inside the step leaves
// the step's name as the last line of the log.
template <typename Step>
auto runDriverCheckStep(std::string_view stepName, Step&& step)
{
    LOG("D3D12 driver check: ", stepName);
    auto result = step();
    LOG("D3D12 driver check: ", stepName, " done");
    return result;
}
} // namespace eacp::GPU
