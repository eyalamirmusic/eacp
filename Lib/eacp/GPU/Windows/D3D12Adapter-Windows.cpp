#include "D3D12Adapter.h"

#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/Strings.h>

#include <algorithm>
#include <iterator>
#include <string_view>

namespace eacp::GPU
{
namespace
{
// The user-mode driver's version as Device Manager shows it: four 16-bit parts.
std::string readDriverVersion(IDXGIAdapter1& adapter)
{
    constexpr auto bitsPerPart = 16;
    constexpr auto partMask = std::uint32_t {0xFFFF};

    auto version = LARGE_INTEGER {};

    if (FAILED(adapter.CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
        return "unknown";

    const auto high = static_cast<std::uint32_t>(version.HighPart);
    const auto low = static_cast<std::uint32_t>(version.LowPart);

    return Strings::format("{}.{}.{}.{}",
                           high >> bitsPerPart,
                           high & partMask,
                           low >> bitsPerPart,
                           low & partMask);
}

AdapterDescription describeUnknownAdapter(std::string_view reason)
{
    auto unknown = AdapterDescription {};
    unknown.name = reason;
    return unknown;
}

AdapterDescription describeDxgiAdapter(IDXGIAdapter1& adapter)
{
    auto description = DXGI_ADAPTER_DESC1 {};

    if (FAILED(adapter.GetDesc1(&description)))
        return describeUnknownAdapter("unknown adapter");

    const auto softwareFlag = static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE);

    return {.name = Strings::narrow(description.Description),
            .vendorId = description.VendorId,
            .deviceId = description.DeviceId,
            .dedicatedMemoryBytes = description.DedicatedVideoMemory,
            .driverVersion = readDriverVersion(adapter),
            .apiLevel = {},
            .isSoftware = (description.Flags & softwareFlag) != 0};
}

struct FeatureLevelName
{
    D3D_FEATURE_LEVEL level;
    std::string_view name;
};

// Oldest first, so a runtime that does not know the newest level can be asked
// again without it.
constexpr FeatureLevelName featureLevelNames[] = {
    {D3D_FEATURE_LEVEL_11_0, "11_0"},
    {D3D_FEATURE_LEVEL_11_1, "11_1"},
    {D3D_FEATURE_LEVEL_12_0, "12_0"},
    {D3D_FEATURE_LEVEL_12_1, "12_1"},
    {D3D_FEATURE_LEVEL_12_2, "12_2"},
};

std::string nameFeatureLevel(D3D_FEATURE_LEVEL level)
{
    for (const auto& entry: featureLevelNames)
        if (entry.level == level)
            return Strings::concat("D3D12 feature level ", entry.name);

    return Strings::format("D3D12 feature level 0x{:x}", static_cast<int>(level));
}

std::string readFeatureLevel(ID3D12Device* device)
{
    D3D_FEATURE_LEVEL levels[std::size(featureLevelNames)] = {};
    std::transform(std::begin(featureLevelNames),
                   std::end(featureLevelNames),
                   std::begin(levels),
                   [](const auto& entry) { return entry.level; });

    for (auto count = static_cast<UINT>(std::size(levels)); count > 0; --count)
    {
        auto query = D3D12_FEATURE_DATA_FEATURE_LEVELS {};
        query.NumFeatureLevels = count;
        query.pFeatureLevelsRequested = levels;

        if (SUCCEEDED(device->CheckFeatureSupport(
                D3D12_FEATURE_FEATURE_LEVELS, &query, sizeof(query))))
            return nameFeatureLevel(query.MaxSupportedFeatureLevel);
    }

    return "D3D12 feature level unknown";
}
} // namespace

// Found by LUID rather than by re-running adapter selection, so it is the
// adapter D3D12CreateDevice actually picked, WARP included.
winrt::com_ptr<IDXGIAdapter1> findAdapterOf(ID3D12Device* device)
{
    auto factory = winrt::com_ptr<IDXGIFactory4>();
    auto adapter = winrt::com_ptr<IDXGIAdapter1>();

    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory.put_void()))
        || FAILED(factory->EnumAdapterByLuid(
            device->GetAdapterLuid(), __uuidof(IDXGIAdapter1), adapter.put_void())))
        return nullptr;

    return adapter;
}

void logEveryAdapter()
{
    auto factory = winrt::com_ptr<IDXGIFactory1>();

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void())))
    {
        LOG("D3D12: no DXGI factory, so no adapter list");
        return;
    }

    for (auto index = UINT {0};; ++index)
    {
        auto adapter = winrt::com_ptr<IDXGIAdapter1>();

        if (FAILED(factory->EnumAdapters1(index, adapter.put())))
            return;

        LOG("D3D12: adapter ",
            index,
            ": ",
            describeForLog(describeDxgiAdapter(*adapter)));
    }
}

AdapterDescription describeDeviceAdapter(ID3D12Device* device)
{
    if (device == nullptr)
        return describeUnknownAdapter("no D3D12 device");

    auto adapter = findAdapterOf(device);

    if (adapter == nullptr)
        return describeUnknownAdapter("unknown adapter");

    auto description = describeDxgiAdapter(*adapter);
    description.apiLevel = readFeatureLevel(device);
    return description;
}
} // namespace eacp::GPU
